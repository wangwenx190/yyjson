#ifndef YYJSON_HPP
#define YYJSON_HPP

#if defined(YYJSON_FREESTANDING) && YYJSON_FREESTANDING
/* The C header's fallback macros must not leak into C++ standard headers. */
#pragma push_macro("memcpy")
#pragma push_macro("memmove")
#pragma push_macro("memset")
#pragma push_macro("memcmp")
#pragma push_macro("strlen")
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#endif
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string_view>

#include "yyjson.h"

#if YYJSON_FREESTANDING
#pragma pop_macro("strlen")
#pragma pop_macro("memcmp")
#pragma pop_macro("memset")
#pragma pop_macro("memmove")
#pragma pop_macro("memcpy")
#endif

namespace yyjson {

namespace detail {

template <typename Entry, yyjson_type Type>
class children_view;

inline const char *string_data(std::string_view text) noexcept {
    return text.data() ? text.data() : "";
}

struct doc_deleter {
    void operator()(yyjson_doc *doc) const noexcept { yyjson_doc_free(doc); }
};

struct mut_doc_deleter {
    void operator()(yyjson_mut_doc *doc) const noexcept {
        yyjson_mut_doc_free(doc);
    }
};

#if !YYJSON_DISABLE_READER || !YYJSON_DISABLE_WRITER
struct workspace_deleter {
    void operator()(yyjson_workspace *work) const noexcept {
        yyjson_workspace_free(work);
    }
};
#endif

#if !YYJSON_DISABLE_WRITER
/* Keep allocation and deallocation in the same runtime, including DLL
   builds and builds configured with a custom C default allocator. */
inline void *allocate(void *, size_t size) { return std::malloc(size); }
inline void *reallocate(void *, void *ptr, size_t, size_t size) {
    return std::realloc(ptr, size);
}
inline void deallocate(void *, void *ptr) { std::free(ptr); }
inline constexpr yyjson_alc allocator{allocate, reallocate, deallocate, nullptr};

struct buffer_deleter {
    void (*free)(void *, void *) = nullptr;
    void *ctx = nullptr;
    void operator()(char *data) const noexcept { free(ctx, data); }
};
#endif

} // namespace detail

class value;
struct member;
using array_view = detail::children_view<value, YYJSON_TYPE_ARR>;
using object_view = detail::children_view<member, YYJSON_TYPE_OBJ>;

/** A borrowed value. Keep its document and any in-situ input alive. */
class value {
public:
    explicit value(const yyjson_val *ptr = nullptr) noexcept : ptr_(ptr) {}

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    const yyjson_val *native_handle() const noexcept { return ptr_; }

    yyjson_type type() const noexcept { return yyjson_get_type(ptr_); }
    yyjson_subtype subtype() const noexcept { return yyjson_get_subtype(ptr_); }
    size_t size() const noexcept { return yyjson_get_len(ptr_); }

    bool as_bool() const noexcept { return yyjson_get_bool(ptr_); }
    uint64_t as_uint() const noexcept { return yyjson_get_uint(ptr_); }
    int64_t as_sint() const noexcept { return yyjson_get_sint(ptr_); }
    double as_real() const noexcept { return yyjson_get_real(ptr_); }
    double as_num() const noexcept { return yyjson_get_num(ptr_); }

    std::string_view as_string() const noexcept {
        const char *text = yyjson_get_str(ptr_);
        return text ? std::string_view(text, size()) : std::string_view{};
    }

    std::string_view as_raw() const noexcept {
        const char *text = yyjson_get_raw(ptr_);
        return text ? std::string_view(text, size()) : std::string_view{};
    }

    value operator[](size_t index) const noexcept {
        return value(yyjson_arr_get(ptr_, index));
    }

    value operator[](std::string_view key) const noexcept {
        return value(yyjson_obj_getn(ptr_, detail::string_data(key), key.size()));
    }

    array_view elements() const noexcept;
    object_view members() const noexcept;

private:
    const yyjson_val *ptr_;
};

struct member {
    std::string_view key;
    value val;
};

namespace detail {

template <typename Entry, yyjson_type Type>
class children_view {
public:
    class iterator {
    public:
        using value_type = Entry;
        using reference = Entry;
        using pointer = void;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        iterator() noexcept = default;
        explicit iterator(const yyjson_val *container) noexcept
            : iterator(container, yyjson_get_type(container) == Type
                                      ? unsafe_yyjson_get_len(container) : 0) {}

        Entry operator*() const noexcept {
            if constexpr (Type == YYJSON_TYPE_OBJ) {
                return {std::string_view(unsafe_yyjson_get_str(cur_),
                                         unsafe_yyjson_get_len(cur_)),
                        value(cur_ + 1)};
            } else {
                return value(cur_);
            }
        }
        iterator &operator++() noexcept {
            if constexpr (Type == YYJSON_TYPE_OBJ) {
                if (--remaining_ == 0) return *this;
                const auto *val = cur_ + 1;
                const size_t step = unsafe_yyjson_is_ctn(val)
                                  ? val->uni.ofs : sizeof(yyjson_val);
                // Combine the key slot and value span before advancing.
                cur_ = reinterpret_cast<const yyjson_val *>(
                    reinterpret_cast<const char *>(cur_) + (step + sizeof(yyjson_val)));
            } else {
                cur_ = unsafe_yyjson_get_next(cur_);
                --remaining_;
            }
            return *this;
        }
        iterator operator++(int) noexcept {
            auto prev = *this;
            ++*this;
            return prev;
        }
        bool operator==(iterator other) const noexcept {
            return remaining_ == other.remaining_ &&
                   (remaining_ == 0 || cur_ == other.cur_);
        }
        bool operator!=(iterator other) const noexcept { return !(*this == other); }

    private:
        iterator(const yyjson_val *container, size_t size) noexcept
            : cur_(size ? unsafe_yyjson_get_first(container) : nullptr),
              remaining_(size) {}

        const yyjson_val *cur_ = nullptr;
        size_t remaining_ = 0;
        friend class children_view;
    };

    explicit children_view(const yyjson_val *ptr) noexcept
        : ptr_(yyjson_get_type(ptr) == Type ? ptr : nullptr) {}

    iterator begin() const noexcept { return iterator(ptr_, size()); }
    iterator end() const noexcept { return {}; }
    size_t size() const noexcept { return ptr_ ? unsafe_yyjson_get_len(ptr_) : 0; }

private:
    const yyjson_val *ptr_;
};

} // namespace detail

inline array_view value::elements() const noexcept { return array_view(ptr_); }
inline object_view value::members() const noexcept { return object_view(ptr_); }

#if !YYJSON_DISABLE_WRITER
/** Owns the C writer's output without copying it into a std::string. */
class text {
public:
    text() noexcept = default;

    explicit operator bool() const noexcept { return data_ != nullptr; }
    const char *data() const noexcept { return data_.get(); }
    size_t size() const noexcept { return data_ ? len_ : 0; }
    std::string_view view() const noexcept {
        return data_ ? std::string_view(data_.get(), len_) : std::string_view{};
    }

private:
    text(char *data, size_t len, const yyjson_alc &alc) noexcept
        : data_(data, detail::buffer_deleter{alc.free, alc.ctx}), len_(len) {}

    template <auto Write, typename Doc>
    static text write(Doc *doc, yyjson_write_flag flags, const yyjson_alc *alc,
                      yyjson_write_err *err) noexcept {
        const yyjson_alc &used = alc ? *alc : detail::allocator;
        size_t len = 0;
        char *data = Write(doc, flags, &used, &len, err);
        return text(data, len, used);
    }

    std::unique_ptr<char, detail::buffer_deleter> data_{nullptr};
    size_t len_ = 0;

    friend class document;
    friend class mutable_document;
};
#endif

/** Move-only owner of an immutable C document. */
class document {
public:
    explicit document(yyjson_doc *ptr = nullptr) noexcept : ptr_(ptr) {}

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    yyjson_doc *native_handle() const noexcept { return ptr_.get(); }
    yyjson_doc *release() noexcept { return ptr_.release(); }
    value root() const noexcept { return value(yyjson_doc_get_root(ptr_.get())); }

#if !YYJSON_DISABLE_READER
    static document parse(std::string_view input, yyjson_read_flag flags = 0,
                          const yyjson_alc *alc = nullptr,
                          yyjson_read_err *err = nullptr) noexcept {
        flags &= ~YYJSON_READ_INSITU;
        // A default-constructed view should report the same error as an empty string.
        const char *data = err ? detail::string_data(input) : input.data();
        return document(yyjson_read_opts(const_cast<char *>(data), input.size(),
                                         flags, alc, err));
    }

    /** Input needs YYJSON_PADDING_SIZE writable bytes past its logical end. */
    static document parse_in_situ(char *input, size_t len,
                                 yyjson_read_flag flags = 0,
                                 const yyjson_alc *alc = nullptr,
                                 yyjson_read_err *err = nullptr) noexcept {
        return document(yyjson_read_opts(input, len, flags | YYJSON_READ_INSITU,
                                         alc, err));
    }

#if !YYJSON_FREESTANDING && !YYJSON_DISABLE_FILE
    static document parse_file(const char *path, yyjson_read_flag flags = 0,
                              const yyjson_alc *alc = nullptr,
                              yyjson_read_err *err = nullptr) noexcept {
        return document(yyjson_read_file(path, flags, alc, err));
    }
#endif
#endif

#if !YYJSON_DISABLE_WRITER
    text dump(yyjson_write_flag flags = 0, const yyjson_alc *alc = nullptr,
              yyjson_write_err *err = nullptr) const noexcept {
        return text::write<yyjson_write_opts>(ptr_.get(), flags, alc, err);
    }
#endif

private:
    std::unique_ptr<yyjson_doc, detail::doc_deleter> ptr_;
};

/** Move-only owner of a mutable C document. Values belong to its arena. */
class mutable_document {
public:
    explicit mutable_document(yyjson_mut_doc *ptr = nullptr) noexcept : ptr_(ptr) {}

    static mutable_document create(const yyjson_alc *alc = nullptr) noexcept {
        return mutable_document(yyjson_mut_doc_new(alc));
    }

    static mutable_document copy(const document &doc,
                                 const yyjson_alc *alc = nullptr) noexcept {
        return mutable_document(yyjson_doc_mut_copy(doc.native_handle(), alc));
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    yyjson_mut_doc *native_handle() const noexcept { return ptr_.get(); }
    yyjson_mut_doc *release() noexcept { return ptr_.release(); }
    yyjson_mut_val *root() const noexcept { return yyjson_mut_doc_get_root(ptr_.get()); }

    void set_root(yyjson_mut_val *val) noexcept {
        yyjson_mut_doc_set_root(ptr_.get(), val);
    }

    yyjson_mut_val *make_array() noexcept { return yyjson_mut_arr(ptr_.get()); }
    yyjson_mut_val *make_object() noexcept { return yyjson_mut_obj(ptr_.get()); }
    yyjson_mut_val *make_null() noexcept { return yyjson_mut_null(ptr_.get()); }
    yyjson_mut_val *make_bool(bool val) noexcept { return yyjson_mut_bool(ptr_.get(), val); }
    yyjson_mut_val *make_uint(uint64_t val) noexcept { return yyjson_mut_uint(ptr_.get(), val); }
    yyjson_mut_val *make_sint(int64_t val) noexcept { return yyjson_mut_sint(ptr_.get(), val); }
    yyjson_mut_val *make_real(double val) noexcept { return yyjson_mut_real(ptr_.get(), val); }
    yyjson_mut_val *make_string(std::string_view val) noexcept {
        return yyjson_mut_strncpy(ptr_.get(), detail::string_data(val), val.size());
    }

    /** Like the C API, container and value must belong to this document. */
    bool add(yyjson_mut_val *obj, std::string_view key, yyjson_mut_val *val) noexcept {
        if (!val || !yyjson_mut_is_obj(obj)) return false;
        return yyjson_mut_obj_add(obj, make_string(key), val);
    }

    bool append(yyjson_mut_val *arr, yyjson_mut_val *val) noexcept {
        return yyjson_mut_arr_append(arr, val);
    }

#if !YYJSON_DISABLE_WRITER
    text dump(yyjson_write_flag flags = 0, const yyjson_alc *alc = nullptr,
              yyjson_write_err *err = nullptr) const noexcept {
        return text::write<yyjson_mut_write_opts>(ptr_.get(), flags, alc, err);
    }
#endif

private:
    std::unique_ptr<yyjson_mut_doc, detail::mut_doc_deleter> ptr_;
};

#if !YYJSON_DISABLE_READER || !YYJSON_DISABLE_WRITER
/** Move-only owner of reusable reading and writing buffers. */
class workspace {
public:
    explicit workspace(yyjson_workspace *ptr = nullptr) noexcept : ptr_(ptr) {}

    static workspace create(size_t cache_limit, const yyjson_alc *alc = nullptr) noexcept {
        return workspace(yyjson_workspace_new(cache_limit, alc));
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    yyjson_workspace *native_handle() const noexcept { return ptr_.get(); }
    void reset() noexcept { yyjson_workspace_reset(ptr_.get()); }

#if !YYJSON_DISABLE_READER
    /** The returned root borrows until the next parse, reset, or destruction. */
    value parse(std::string_view input, yyjson_read_flag flags = 0,
                yyjson_read_err *err = nullptr) & noexcept {
        const char *data = err ? detail::string_data(input) : input.data();
        const auto *doc = yyjson_workspace_read(ptr_.get(), data, input.size(), flags, err);
        return value(yyjson_doc_get_root(doc));
    }

    /** Capacity includes writable padding. Keep input alive with the result. */
    value parse_in_situ(char *input, size_t len, size_t capacity,
                        yyjson_read_flag flags = 0,
                        yyjson_read_err *err = nullptr) & noexcept {
        const auto *doc = yyjson_workspace_read_in_situ(ptr_.get(), input, len,
                                                       capacity, flags, err);
        return value(yyjson_doc_get_root(doc));
    }
#endif

#if !YYJSON_DISABLE_WRITER
    /** The result borrows until the next dump, reset, or destruction. */
    std::string_view dump(value root, yyjson_write_flag flags = 0,
                          yyjson_write_err *err = nullptr) & noexcept {
        size_t len = 0;
        const char *data = yyjson_workspace_write(ptr_.get(), root.native_handle(),
                                                flags, &len, err);
        return data ? std::string_view(data, len) : std::string_view{};
    }
#endif

private:
    std::unique_ptr<yyjson_workspace, detail::workspace_deleter> ptr_;
};
#endif

} // namespace yyjson

#endif // YYJSON_HPP
