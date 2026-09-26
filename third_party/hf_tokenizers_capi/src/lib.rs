use std::cell::RefCell;
use std::ffi::{c_char, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::ptr;
use std::slice;

use tokenizers::{PaddingStrategy, Tokenizer};

const ABI_VERSION: u32 = 1;
const OK: i32 = 0;
const ERR_NULL_POINTER: i32 = 1;
const ERR_INVALID_ARG: i32 = 2;
const ERR_INVALID_UTF8: i32 = 3;
const ERR_IO: i32 = 4;
const ERR_TOKENIZER_LOAD: i32 = 5;
const ERR_ENCODE_FAILED: i32 = 6;
const ERR_OUT_OF_BOUNDS: i32 = 7;
const ERR_PANIC: i32 = 8;
const ERR_INTERNAL: i32 = 9;

macro_rules! ffi_status {
    ($body:block) => {{
        // Rust panic 不得跨越 C ABI；统一转换为稳定错误码和线程本地错误信息。
        match catch_unwind(AssertUnwindSafe(|| -> i32 { $body })) {
            Ok(code) => code,
            Err(payload) => {
                let message = if let Some(message) = payload.downcast_ref::<&str>() {
                    *message
                } else if let Some(message) = payload.downcast_ref::<String>() {
                    message.as_str()
                } else {
                    "unknown Rust panic"
                };
                set_last_error(format!(
                    "Rust panic at the tokenizer FFI boundary: {message}"
                ));
                ERR_PANIC
            }
        }
    }};
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HfByteSpan {
    ptr: *const u8,
    len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HfEncodeOptions {
    max_length: usize,
    truncation: u8,
    padding: u8,
    pad_to_longest_in_batch: u8,
    add_special_tokens: u8,
    reserved: [u8; 4],
}

#[repr(C)]
pub struct HfTokenizer {
    tokenizer: Tokenizer,
}

#[repr(C)]
pub struct HfTokenizedBatch {
    batch_size: usize,
    sequence_length: usize,
    input_ids: Vec<i64>,
    attention_mask: Vec<i64>,
    token_type_ids: Vec<i64>,
}

fn set_last_error(message: impl AsRef<str>) {
    let sanitized = message.as_ref().replace('\0', "\\0");
    let value = CString::new(sanitized).unwrap_or_default();
    LAST_ERROR.with(|slot| *slot.borrow_mut() = value);
}

fn clear_last_error() {
    LAST_ERROR.with(|slot| *slot.borrow_mut() = CString::default());
}

unsafe fn span_as_str<'a>(span: HfByteSpan) -> Result<&'a str, i32> {
    if span.ptr.is_null() && span.len != 0 {
        set_last_error("byte span has a null pointer with a non-zero length");
        return Err(ERR_NULL_POINTER);
    }
    let bytes = if span.len == 0 {
        &[]
    } else {
        slice::from_raw_parts(span.ptr, span.len)
    };
    std::str::from_utf8(bytes).map_err(|error| {
        set_last_error(format!("input is not valid UTF-8: {error}"));
        ERR_INVALID_UTF8
    })
}

fn configured_tokenizer(
    tokenizer: &Tokenizer,
    options: &HfEncodeOptions,
) -> Result<Tokenizer, i32> {
    if options.max_length == 0 && options.truncation != 0 {
        set_last_error("max_length must be greater than zero when truncation is enabled");
        return Err(ERR_INVALID_ARG);
    }

    // 每次编码使用独立快照，只覆盖请求级策略，保留 tokenizer.json 中的 pad id/token 等模型契约。
    let mut configured = tokenizer.clone();
    if options.truncation != 0 {
        let mut params = configured.get_truncation().cloned().unwrap_or_default();
        params.max_length = options.max_length;
        configured.with_truncation(Some(params)).map_err(|error| {
            set_last_error(format!("failed to configure truncation: {error}"));
            ERR_INVALID_ARG
        })?;
    } else {
        configured.with_truncation(None).map_err(|error| {
            set_last_error(format!("failed to disable truncation: {error}"));
            ERR_INTERNAL
        })?;
    }

    if options.padding != 0 {
        let strategy = if options.pad_to_longest_in_batch != 0 {
            PaddingStrategy::BatchLongest
        } else {
            if options.max_length == 0 {
                set_last_error("max_length must be greater than zero for fixed padding");
                return Err(ERR_INVALID_ARG);
            }
            PaddingStrategy::Fixed(options.max_length)
        };
        let mut params = configured.get_padding().cloned().unwrap_or_default();
        params.strategy = strategy;
        configured.with_padding(Some(params));
    } else {
        configured.with_padding(None);
    }
    Ok(configured)
}

fn materialize(encodings: Vec<tokenizers::Encoding>) -> Result<HfTokenizedBatch, i32> {
    let batch_size = encodings.len();
    let sequence_length = encodings.first().map_or(0, |encoding| encoding.len());
    if encodings
        .iter()
        .any(|encoding| encoding.len() != sequence_length)
    {
        set_last_error("tokenizer returned variable-length rows without padding");
        return Err(ERR_INTERNAL);
    }

    let total = batch_size.checked_mul(sequence_length).ok_or_else(|| {
        set_last_error("tokenized batch size overflow");
        ERR_OUT_OF_BOUNDS
    })?;
    let mut input_ids = Vec::with_capacity(total);
    let mut attention_mask = Vec::with_capacity(total);
    let mut token_type_ids = Vec::with_capacity(total);
    for encoding in encodings {
        input_ids.extend(encoding.get_ids().iter().map(|&value| i64::from(value)));
        attention_mask.extend(
            encoding
                .get_attention_mask()
                .iter()
                .map(|&value| i64::from(value)),
        );
        token_type_ids.extend(
            encoding
                .get_type_ids()
                .iter()
                .map(|&value| i64::from(value)),
        );
    }
    Ok(HfTokenizedBatch {
        batch_size,
        sequence_length,
        input_ids,
        attention_mask,
        token_type_ids,
    })
}

#[no_mangle]
pub extern "C" fn hf_tokenizers_abi_version() -> u32 {
    ABI_VERSION
}

#[no_mangle]
pub extern "C" fn hf_tokenizers_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[no_mangle]
/// Creates a tokenizer from a UTF-8 file path.
///
/// # Safety
/// `path.ptr` must reference `path.len` readable bytes when the length is non-zero, and `output`
/// must reference writable storage for one tokenizer pointer.
pub unsafe extern "C" fn hf_tokenizer_create_from_file(
    path: HfByteSpan,
    output: *mut *mut HfTokenizer,
) -> i32 {
    ffi_status!({
        if output.is_null() {
            set_last_error("output tokenizer pointer is null");
            return ERR_NULL_POINTER;
        }
        *output = ptr::null_mut();
        let path = match span_as_str(path) {
            Ok(path) if !path.is_empty() => path,
            Ok(_) => {
                set_last_error("tokenizer path is empty");
                return ERR_INVALID_ARG;
            }
            Err(code) => return code,
        };
        if !Path::new(path).is_file() {
            set_last_error(format!("tokenizer file does not exist: {path}"));
            return ERR_IO;
        }
        match Tokenizer::from_file(path) {
            Ok(tokenizer) => {
                *output = Box::into_raw(Box::new(HfTokenizer { tokenizer }));
                clear_last_error();
                OK
            }
            Err(error) => {
                set_last_error(format!("failed to load tokenizer: {error}"));
                ERR_TOKENIZER_LOAD
            }
        }
    })
}

#[no_mangle]
/// Clones an existing tokenizer handle.
///
/// # Safety
/// `tokenizer` must be a live handle created by this library, and `output` must reference writable
/// storage for one tokenizer pointer.
pub unsafe extern "C" fn hf_tokenizer_clone(
    tokenizer: *const HfTokenizer,
    output: *mut *mut HfTokenizer,
) -> i32 {
    ffi_status!({
        if tokenizer.is_null() || output.is_null() {
            set_last_error("tokenizer or output pointer is null");
            return ERR_NULL_POINTER;
        }
        *output = Box::into_raw(Box::new(HfTokenizer {
            tokenizer: (*tokenizer).tokenizer.clone(),
        }));
        clear_last_error();
        OK
    })
}

#[no_mangle]
/// Destroys a tokenizer handle. A null pointer is accepted.
///
/// # Safety
/// A non-null `tokenizer` must be a live handle created by this library and must be destroyed once.
pub unsafe extern "C" fn hf_tokenizer_destroy(tokenizer: *mut HfTokenizer) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !tokenizer.is_null() {
            drop(Box::from_raw(tokenizer));
        }
    }))
    .map_err(|_| set_last_error("Rust panic while destroying a tokenizer handle"));
}

#[no_mangle]
/// Encodes one UTF-8 text span.
///
/// # Safety
/// All non-null pointers must remain valid for the duration of the call. `tokenizer` must be a live
/// handle created by this library, and `output` must reference writable storage for one batch pointer.
pub unsafe extern "C" fn hf_tokenizer_encode(
    tokenizer: *const HfTokenizer,
    text: HfByteSpan,
    options: *const HfEncodeOptions,
    output: *mut *mut HfTokenizedBatch,
) -> i32 {
    ffi_status!({
        if tokenizer.is_null() || options.is_null() || output.is_null() {
            set_last_error("tokenizer, options, or output pointer is null");
            return ERR_NULL_POINTER;
        }
        *output = ptr::null_mut();
        let text = match span_as_str(text) {
            Ok(text) => text,
            Err(code) => return code,
        };
        let configured = match configured_tokenizer(&(*tokenizer).tokenizer, &*options) {
            Ok(tokenizer) => tokenizer,
            Err(code) => return code,
        };
        match configured.encode(text, (*options).add_special_tokens != 0) {
            Ok(encoding) => match materialize(vec![encoding]) {
                Ok(batch) => {
                    *output = Box::into_raw(Box::new(batch));
                    clear_last_error();
                    OK
                }
                Err(code) => code,
            },
            Err(error) => {
                set_last_error(format!("failed to encode text: {error}"));
                ERR_ENCODE_FAILED
            }
        }
    })
}

#[no_mangle]
/// Encodes a non-empty array of UTF-8 text spans.
///
/// # Safety
/// `texts` must reference `text_count` readable spans, each span must reference its declared bytes,
/// `tokenizer` must be live, and `options` and `output` must be valid for the duration of the call.
pub unsafe extern "C" fn hf_tokenizer_encode_batch(
    tokenizer: *const HfTokenizer,
    texts: *const HfByteSpan,
    text_count: usize,
    options: *const HfEncodeOptions,
    output: *mut *mut HfTokenizedBatch,
) -> i32 {
    ffi_status!({
        if tokenizer.is_null() || texts.is_null() || options.is_null() || output.is_null() {
            set_last_error("tokenizer, texts, options, or output pointer is null");
            return ERR_NULL_POINTER;
        }
        if text_count == 0 {
            set_last_error("text batch is empty");
            return ERR_INVALID_ARG;
        }
        *output = ptr::null_mut();
        let spans = slice::from_raw_parts(texts, text_count);
        let mut inputs = Vec::with_capacity(text_count);
        for &span in spans {
            match span_as_str(span) {
                Ok(text) => inputs.push(text),
                Err(code) => return code,
            }
        }
        let configured = match configured_tokenizer(&(*tokenizer).tokenizer, &*options) {
            Ok(tokenizer) => tokenizer,
            Err(code) => return code,
        };
        match configured.encode_batch(inputs, (*options).add_special_tokens != 0) {
            Ok(encodings) => match materialize(encodings) {
                Ok(batch) => {
                    *output = Box::into_raw(Box::new(batch));
                    clear_last_error();
                    OK
                }
                Err(code) => code,
            },
            Err(error) => {
                set_last_error(format!("failed to encode text batch: {error}"));
                ERR_ENCODE_FAILED
            }
        }
    })
}

#[no_mangle]
/// Destroys a tokenized batch. A null pointer is accepted.
///
/// # Safety
/// A non-null `batch` must be a live handle created by this library and must be destroyed once.
pub unsafe extern "C" fn hf_tokenized_batch_destroy(batch: *mut HfTokenizedBatch) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !batch.is_null() {
            drop(Box::from_raw(batch));
        }
    }))
    .map_err(|_| set_last_error("Rust panic while destroying a tokenized batch"));
}

unsafe fn write_usize(batch: *const HfTokenizedBatch, output: *mut usize, value: usize) -> i32 {
    if batch.is_null() || output.is_null() {
        set_last_error("batch or output pointer is null");
        return ERR_NULL_POINTER;
    }
    *output = value;
    clear_last_error();
    OK
}

unsafe fn write_data(batch: *const HfTokenizedBatch, output: *mut *const i64, data: &[i64]) -> i32 {
    if batch.is_null() || output.is_null() {
        set_last_error("batch or output pointer is null");
        return ERR_NULL_POINTER;
    }
    *output = if data.is_empty() {
        ptr::null()
    } else {
        data.as_ptr()
    };
    clear_last_error();
    OK
}

#[no_mangle]
/// Returns the number of rows in a tokenized batch.
///
/// # Safety
/// `batch` must be a live batch handle, and `output` must reference writable `usize` storage.
pub unsafe extern "C" fn hf_tokenized_batch_batch_size(
    batch: *const HfTokenizedBatch,
    output: *mut usize,
) -> i32 {
    ffi_status!({
        if batch.is_null() {
            set_last_error("batch pointer is null");
            return ERR_NULL_POINTER;
        }
        write_usize(batch, output, (*batch).batch_size)
    })
}

#[no_mangle]
/// Returns the padded sequence length of a tokenized batch.
///
/// # Safety
/// `batch` must be a live batch handle, and `output` must reference writable `usize` storage.
pub unsafe extern "C" fn hf_tokenized_batch_sequence_length(
    batch: *const HfTokenizedBatch,
    output: *mut usize,
) -> i32 {
    ffi_status!({
        if batch.is_null() {
            set_last_error("batch pointer is null");
            return ERR_NULL_POINTER;
        }
        write_usize(batch, output, (*batch).sequence_length)
    })
}

#[no_mangle]
/// Borrows the contiguous input-id buffer owned by a tokenized batch.
///
/// # Safety
/// `batch` must remain alive while the returned buffer is read, and `output` must be writable.
pub unsafe extern "C" fn hf_tokenized_batch_input_ids(
    batch: *const HfTokenizedBatch,
    output: *mut *const i64,
) -> i32 {
    ffi_status!({
        if batch.is_null() {
            set_last_error("batch pointer is null");
            return ERR_NULL_POINTER;
        }
        write_data(batch, output, &(*batch).input_ids)
    })
}

#[no_mangle]
/// Borrows the contiguous attention-mask buffer owned by a tokenized batch.
///
/// # Safety
/// `batch` must remain alive while the returned buffer is read, and `output` must be writable.
pub unsafe extern "C" fn hf_tokenized_batch_attention_mask(
    batch: *const HfTokenizedBatch,
    output: *mut *const i64,
) -> i32 {
    ffi_status!({
        if batch.is_null() {
            set_last_error("batch pointer is null");
            return ERR_NULL_POINTER;
        }
        write_data(batch, output, &(*batch).attention_mask)
    })
}

#[no_mangle]
/// Borrows the contiguous token-type-id buffer owned by a tokenized batch.
///
/// # Safety
/// `batch` must remain alive while the returned buffer is read, and `output` must be writable.
pub unsafe extern "C" fn hf_tokenized_batch_token_type_ids(
    batch: *const HfTokenizedBatch,
    output: *mut *const i64,
) -> i32 {
    ffi_status!({
        if batch.is_null() {
            set_last_error("batch pointer is null");
            return ERR_NULL_POINTER;
        }
        write_data(batch, output, &(*batch).token_type_ids)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};
    use tokenizers::models::wordlevel::WordLevel;
    use tokenizers::pre_tokenizers::whitespace::Whitespace;

    fn fixture_path() -> std::path::PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock moved backwards")
            .as_nanos();
        let path = std::env::temp_dir().join(format!("hf-tokenizer-{unique}.json"));
        let vocab = ahash::AHashMap::from_iter([
            ("[UNK]".to_owned(), 0),
            ("hello".to_owned(), 1),
            ("world".to_owned(), 2),
            ("short".to_owned(), 3),
        ]);
        let model = WordLevel::builder()
            .vocab(vocab)
            .unk_token("[UNK]".to_owned())
            .build()
            .expect("word-level model");
        let mut tokenizer = Tokenizer::new(model);
        tokenizer.with_pre_tokenizer(Some(Whitespace));
        tokenizer
            .save(&path, false)
            .expect("save tokenizer fixture");
        path
    }

    fn span(value: &str) -> HfByteSpan {
        HfByteSpan {
            ptr: value.as_ptr(),
            len: value.len(),
        }
    }

    fn default_options() -> HfEncodeOptions {
        HfEncodeOptions {
            max_length: 8,
            truncation: 1,
            padding: 1,
            pad_to_longest_in_batch: 1,
            add_special_tokens: 1,
            reserved: [0; 4],
        }
    }

    #[test]
    fn ffi_load_clone_and_batch_padding() {
        let fixture = fixture_path();
        let fixture_text = fixture.to_string_lossy();
        let mut tokenizer = ptr::null_mut();
        let status = unsafe { hf_tokenizer_create_from_file(span(&fixture_text), &mut tokenizer) };
        assert_eq!(status, OK);
        assert!(!tokenizer.is_null());

        let mut clone = ptr::null_mut();
        assert_eq!(unsafe { hf_tokenizer_clone(tokenizer, &mut clone) }, OK);
        assert!(!clone.is_null());

        let inputs = [span("hello world"), span("short")];
        let options = default_options();
        let mut batch = ptr::null_mut();
        assert_eq!(
            unsafe {
                hf_tokenizer_encode_batch(
                    clone,
                    inputs.as_ptr(),
                    inputs.len(),
                    &options,
                    &mut batch,
                )
            },
            OK
        );

        let mut batch_size = 0;
        let mut sequence_length = 0;
        assert_eq!(
            unsafe { hf_tokenized_batch_batch_size(batch, &mut batch_size) },
            OK
        );
        assert_eq!(
            unsafe { hf_tokenized_batch_sequence_length(batch, &mut sequence_length) },
            OK
        );
        assert_eq!(batch_size, 2);
        assert_eq!(sequence_length, 2);

        unsafe {
            hf_tokenized_batch_destroy(batch);
            hf_tokenizer_destroy(clone);
            hf_tokenizer_destroy(tokenizer);
        }
        fs::remove_file(fixture).expect("remove tokenizer fixture");
    }

    #[test]
    fn ffi_rejects_invalid_utf8() {
        let invalid = [0xC3_u8, 0x28];
        let span = HfByteSpan {
            ptr: invalid.as_ptr(),
            len: invalid.len(),
        };
        assert!(matches!(
            unsafe { span_as_str(span) },
            Err(ERR_INVALID_UTF8)
        ));
    }
}
