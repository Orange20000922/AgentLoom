#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hf_tokenizer hf_tokenizer_t;
typedef struct hf_tokenized_batch hf_tokenized_batch_t;

// 句柄与批次内存均由 Rust 侧拥有，调用方必须通过对应 destroy 接口释放。

typedef struct hf_byte_span {
    const uint8_t* ptr;
    size_t len;
} hf_byte_span_t;

typedef struct hf_encode_options {
    size_t max_length;
    uint8_t truncation;
    uint8_t padding;
    uint8_t pad_to_longest_in_batch;
    uint8_t add_special_tokens;
    uint8_t reserved[4];
} hf_encode_options_t;

enum hf_tokenizer_status {
    HF_TOKENIZER_OK = 0,
    HF_TOKENIZER_ERR_NULL_POINTER = 1,
    HF_TOKENIZER_ERR_INVALID_ARG = 2,
    HF_TOKENIZER_ERR_INVALID_UTF8 = 3,
    HF_TOKENIZER_ERR_IO = 4,
    HF_TOKENIZER_ERR_TOKENIZER_LOAD = 5,
    HF_TOKENIZER_ERR_ENCODE_FAILED = 6,
    HF_TOKENIZER_ERR_OUT_OF_BOUNDS = 7,
    HF_TOKENIZER_ERR_PANIC = 8,
    HF_TOKENIZER_ERR_INTERNAL = 9,
};

uint32_t hf_tokenizers_abi_version(void);
const char* hf_tokenizers_last_error_message(void);

int32_t hf_tokenizer_create_from_file(hf_byte_span_t path, hf_tokenizer_t** output);
int32_t hf_tokenizer_clone(const hf_tokenizer_t* tokenizer, hf_tokenizer_t** output);
void hf_tokenizer_destroy(hf_tokenizer_t* tokenizer);

int32_t hf_tokenizer_encode(
    const hf_tokenizer_t* tokenizer,
    hf_byte_span_t text,
    const hf_encode_options_t* options,
    hf_tokenized_batch_t** output);

int32_t hf_tokenizer_encode_batch(
    const hf_tokenizer_t* tokenizer,
    const hf_byte_span_t* texts,
    size_t text_count,
    const hf_encode_options_t* options,
    hf_tokenized_batch_t** output);

void hf_tokenized_batch_destroy(hf_tokenized_batch_t* batch);
int32_t hf_tokenized_batch_batch_size(const hf_tokenized_batch_t* batch, size_t* output);
int32_t hf_tokenized_batch_sequence_length(const hf_tokenized_batch_t* batch, size_t* output);
// 返回的数组只在 batch 存活期间有效，调用方不得释放或长期保存其地址。
int32_t hf_tokenized_batch_input_ids(const hf_tokenized_batch_t* batch, const int64_t** output);
int32_t hf_tokenized_batch_attention_mask(const hf_tokenized_batch_t* batch, const int64_t** output);
int32_t hf_tokenized_batch_token_type_ids(const hf_tokenized_batch_t* batch, const int64_t** output);

#ifdef __cplusplus
}
#endif
