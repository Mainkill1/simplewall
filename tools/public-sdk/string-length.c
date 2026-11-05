ULONG_PTR _r_str_getlength_ex (
    _In_reads_or_z_ (max_length) LPCWSTR string,
    _In_ _In_range_ (<= , PR_SIZE_MAX_STRING_LENGTH) ULONG_PTR max_length
)
{
    // The public SDK's SSE path reads outside the string and ignores max_length.
    // This bounded scan is used in all compatibility builds, not only ASan runs.
    ULONG_PTR length = 0;
    while (length < max_length && string[length] != UNICODE_NULL)
        length++;
    return length;
}
