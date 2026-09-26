#include "hf_tokenizers_capi.h"

#include <cstdint>
#include <iostream>

int main() {
    if (hf_tokenizers_abi_version() != 1) {
        std::cerr << "unexpected tokenizer ABI version\n";
        return 1;
    }

    const std::uint8_t invalid_utf8[] = {0xC3, 0x28};
    hf_byte_span_t text{invalid_utf8, sizeof(invalid_utf8)};
    hf_encode_options_t options{};
    options.max_length = 8;
    options.truncation = 1;
    options.padding = 1;
    options.pad_to_longest_in_batch = 1;
    options.add_special_tokens = 1;

    hf_tokenized_batch_t* batch = nullptr;
    const auto status = hf_tokenizer_encode(nullptr, text, &options, &batch);
    if (status != HF_TOKENIZER_ERR_NULL_POINTER || batch != nullptr) {
        std::cerr << "null tokenizer was not rejected safely\n";
        return 2;
    }
    return 0;
}
