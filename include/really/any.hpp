#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdlib>
#include <string_view>
#include <type_traits>


namespace really
{

// get string_view of a typename

namespace typename_impl
{
template <class T>
constexpr std::string_view raw_type_name()
{
#if defined(_MSC_VER)
	return __FUNCSIG__;
#else
	return __PRETTY_FUNCTION__;
#endif
}

constexpr auto test_name = raw_type_name<double>();
constexpr size_t prefix_length = test_name.find("double");
static_assert(prefix_length != std::string_view::npos,
			  "cannot extract typename from function signature");
constexpr size_t suffix_length =
	test_name.size() - prefix_length - std::string_view("double").size();
} // namespace typename_impl

template <class T>
consteval std::string_view type_name()
{
	using namespace typename_impl;
	std::string_view name = raw_type_name<T>();
	return std::string_view(name.data() + prefix_length,
							name.size() - prefix_length - suffix_length);
}


// A std::type_info replacement that works across DLL/so boundaries
class type_info
{
public:
	inline constexpr std::string_view name() const noexcept { return typename_; }

	inline /*constexpr*/ size_t hash_code() const noexcept
	{
		return std::hash<std::string_view>{}(typename_);
	}

	inline constexpr bool operator==(const type_info& other) const noexcept
	{
		return typename_ == other.typename_;
	}

#ifdef STRING_VIEW_HAS_SPACESHIP_OPERATOR
	inline constexpr auto operator<=>(const type_info& other) const noexcept
	{
		return typename_ <=> other.typename_;
	}
#endif

	inline constexpr bool before(const type_info& other) const noexcept { return typename_ < other.typename_; }

private:
	template <class T>
	friend constexpr type_info get_type_info();

	std::string_view typename_;
};

template <class T>
constexpr type_info get_type_info()
{
	type_info result;
	result.typename_ = type_name<T>();
	return result;
}

}  // namespace really

template <>
struct std::hash<really::type_info>
{
	std::size_t operator()(const really::type_info& ti) const noexcept
	{
		return ti.hash_code();
	}
};

// type-erased operations library
namespace really::typeops
{
using unary_typeop_t = void (*)(void* ptr);
using copy_typeop_t = void (*)(void* dest, const void* src);
using move_typeop_t = void (*)(void* dest, void* src);

namespace typeop_impl
{
template <class T>
constexpr unary_typeop_t make_default_construct()
{
	if constexpr (std::is_default_constructible_v<T>)
	{
		return [](void* ptr) { new (ptr) T(); };
	}
	return nullptr;
}

template <class T>
constexpr unary_typeop_t make_destruct()
{
	return [](void* ptr) { static_cast<T*>(ptr)->~T(); };
}

template <class T>
constexpr copy_typeop_t make_copy_construct()
{
	if constexpr (std::is_copy_constructible_v<T>)
	{
		return [](void* dest, const void* src) { new (dest) T(*static_cast<const T*>(src)); };
	}
	return nullptr;
}

template <class T>
constexpr copy_typeop_t make_copy_assign()
{
	if constexpr (std::is_copy_assignable_v<T>)
	{
		return [](void* dest, const void* src) {
			*static_cast<T*>(dest) = *static_cast<const T*>(src);
		};
	}
	return nullptr;
}

template <class T>
constexpr move_typeop_t make_move_construct()
{
	if constexpr (std::is_move_constructible_v<T>)
	{
		return [](void* dest, void* src) { new (dest) T(std::move(*static_cast<T*>(src))); };
	}
	return nullptr;
}

template <class T>
constexpr move_typeop_t make_move_assign()
{
	if constexpr (std::is_move_assignable_v<T>)
	{
		return [](void* dest, void* src) {
			*static_cast<T*>(dest) = std::move(*static_cast<T*>(src));
		};
	}
	return nullptr;
}
} // namespace typeop_impl

template <class T>
inline unary_typeop_t default_construct = typeop_impl::make_default_construct<T>();

template <class T>
inline unary_typeop_t destruct = typeop_impl::make_destruct<T>();

template <class T>
inline copy_typeop_t copy_construct = typeop_impl::make_copy_construct<T>();

template <class T>
inline copy_typeop_t copy_assign = typeop_impl::make_copy_assign<T>();

template <class T>
inline move_typeop_t move_construct = typeop_impl::make_move_construct<T>();

template <class T>
inline move_typeop_t move_assign = typeop_impl::make_move_assign<T>();

}  // namespace really


// any library
namespace really
{
enum class any_copy_support
{
	no_copy_or_move,
	move_only,
	copy_and_move,
};

namespace detail
{
template <class T>
concept any_storage = requires(T storage, T* storage_ptr) {
	storage.allocate(size_t());
	storage.free();
	{
		storage.get_storage()
	} -> std::convertible_to<void*>;
	{
		std::as_const(storage).get_storage()
	} -> std::convertible_to<const void*>;
	{
		T::can_always_swap
	} -> std::convertible_to<bool>;
	{
		storage.try_swap(storage_ptr)
	} -> std::convertible_to<bool>;
};

struct any_heap_storage
{
	void allocate(size_t size) { data_ = malloc(size); }

	void free()
	{
		::free(data_);
		data_ = nullptr;
	}

	void* get_storage() const { return data_; }

	constexpr static bool can_always_swap = true;
	bool try_swap(any_heap_storage* other)
	{
		std::swap(data_, other->data_);
		return true;
	}

private:
	void* data_ = nullptr;
};

template <size_t Size>
struct any_local_storage
{
	void allocate(size_t size)
	{
		assert(size <= Size);
		is_empty_ = false;
	}

	void free() { is_empty_ = true; }

	void* get_storage() { return is_empty_ ? nullptr : &data_[0]; }
	const void* get_storage() const { return is_empty_ ? nullptr : &data_[0]; }

	constexpr static bool can_always_swap = false;
	bool try_swap(any_local_storage* other) { return false; }

private:
	char data_[Size];
	bool is_empty_ = true;
};

template <size_t Size>
struct any_small_buffer_storage
{
	any_small_buffer_storage() { state_ = state::empty; }

	void allocate(size_t size)
	{
		assert(state_ == state::empty);
		if (size <= sizeof(data_))
		{
			state_ = state::local;
		}
		else
		{
			ptr_ = malloc(size);
			state_ = state::heap;
		}
	}

	void free()
	{
		if (state_ == state::heap)
		{
			::free(ptr_);
		}
		state_ = state::empty;
	}

	void* get_storage()
	{
		switch (state_)
		{
		default:
		case state::empty:
			return nullptr;
		case state::heap:
			return ptr_;
		case state::local:
			return &data_[0];
		}
	}

	const void* get_storage() const
	{
		return const_cast<any_small_buffer_storage*>(this)->get_storage();
	}

	constexpr static bool can_always_swap = false;
	bool try_swap(any_small_buffer_storage* other)
	{
		if (state_ == state::heap && other->state_ == state::heap)
		{
			std::swap(ptr_, other->ptr_);
			return true;
		}
		return false;
	}

private:
	enum class state : char
	{
		empty,
		local,
		heap,
	};

	union {
		struct
		{
			char data_[Size < sizeof(void*) ? sizeof(void*) : Size];
			state state_;
		};
		void* ptr_;
	};
};

class any_type_operations
{
public:
	virtual size_t size() const = 0;
	virtual type_info get_type_info() const = 0;
	virtual void copy(void* dest, const void* src) const = 0;
	virtual void copy_assign(void* dest, const void* src) const = 0;
	virtual void move(void* dest, void* src) const = 0;
	virtual void move_assign(void* dest, void* src) const = 0;
	virtual void destruct(void* dest) const = 0;
};

template <class T>
class any_type_operations_impl : public any_type_operations
{
	virtual size_t size() const { return sizeof(T); }
	virtual type_info get_type_info() const { return really::get_type_info<T>(); }

	virtual void copy(void* dest, const void* src) const
	{
		if (auto copy_func = typeops::copy_construct<T>)
		{
			copy_func(dest, src);
		}
	}

	virtual void copy_assign(void* dest, const void* src) const
	{
		if (auto copy_func = typeops::copy_assign<T>)
		{
			copy_func(dest, src);
		}
	}

	virtual void move(void* dest, void* src) const
	{
		if (auto move_func = typeops::move_construct<T>)
		{
			move_func(dest, src);
		}
	}

	virtual void move_assign(void* dest, void* src) const
	{
		if (auto move_func = typeops::move_assign<T>)
		{
			move_func(dest, src);
		}
	}

	virtual void destruct(void* dest) const { typeops::destruct<T>(dest); }
};

template <class T>
constexpr inline any_type_operations_impl<T> type_operations = {};

template <any_storage Storage, any_copy_support CopySupport>
class any_base : Storage
{
	using this_t = any_base<Storage, CopySupport>;
public:
	static constexpr any_copy_support copy_support = CopySupport;

	any_base() = default;
	~any_base() { reset(); }

	any_base(const any_base& other)
		requires(CopySupport == any_copy_support::copy_and_move)
	{
		copy(other);
	}

	any_base(any_base&& other) noexcept
		requires(CopySupport > any_copy_support::no_copy_or_move)
	{
		swap(other);
	}

	template <any_storage OtherStorage, any_copy_support OtherCopySupport>
		requires(std::min(CopySupport, OtherCopySupport) == any_copy_support::copy_and_move)
	any_base(const any_base<OtherStorage, OtherCopySupport>& other)
	{
		copy(other);
	}

	template <any_storage OtherStorage, any_copy_support OtherCopySupport>
		requires(std::min(CopySupport, OtherCopySupport) > any_copy_support::no_copy_or_move)
	any_base(any_base<OtherStorage, OtherCopySupport>&& other) noexcept
	{
		move(other);
	}

	template <class T>
		requires(!std::is_base_of_v<this_t, T> && CopySupport == any_copy_support::copy_and_move && std::is_copy_constructible_v<T>)
	any_base(const T& value)
	{
		emplace<T>(value);
	}

	template <class T>
		requires(!std::is_base_of_v<this_t, T> && CopySupport > any_copy_support::no_copy_or_move && std::is_move_constructible_v<T>)
	any_base(T&& value) noexcept
	{
		emplace<T>(std::move(value));
	}

	any_base& operator=(const any_base& other)
		requires(CopySupport == any_copy_support::copy_and_move)
	{
		copy(other);
		return *this;
	}

	any_base& operator=(any_base&& other) noexcept
		requires(CopySupport > any_copy_support::no_copy_or_move)
	{
		reset();
		swap(other);
		return *this;
	}

	template <any_storage OtherStorage, any_copy_support OtherCopySupport>
		requires(std::min(CopySupport, OtherCopySupport) == any_copy_support::copy_and_move)
	any_base& operator=(const any_base<OtherStorage, OtherCopySupport>& other)
	{
		copy(other);
		return *this;
	}

	template <any_storage OtherStorage, any_copy_support OtherCopySupport>
		requires(std::min(CopySupport, OtherCopySupport) > any_copy_support::no_copy_or_move)
	any_base& operator=(any_base<OtherStorage, OtherCopySupport>&& other) noexcept
	{
		move(other);
		return *this;
	}

	template <class T>
		requires(!std::is_base_of_v<this_t, T>&& CopySupport == any_copy_support::copy_and_move && std::is_copy_constructible_v<T>)
	any_base& operator=(const T& value)
	{
		if (any_ops_ != nullptr && any_ops_->get_type_info() == get_type_info<T>())
		{
			any_ops_->copy_assign(this->get_storage(), &value);
		}
		else
		{
			reset();
			emplace<T>(value);
		}
		return *this;
	}

	template <class T>
		requires(!std::is_base_of_v<this_t, T> && !std::is_lvalue_reference_v<T> &&
				 CopySupport > any_copy_support::no_copy_or_move && std::is_move_constructible_v<T>)
	any_base& operator=(T&& value) noexcept
	{
		if (any_ops_ != nullptr && any_ops_->get_type_info() == get_type_info<T>())
		{
			any_ops_->move_assign(this->get_storage(), &value);
		}
		else
		{
			reset();
			emplace<T>(std::move(value));
		}
		return *this;
	}

	template <class T, class... Args>
	std::decay_t<T>& emplace(Args&&... args)
	{
		reset();

		using value_t = std::decay_t<T>;
		this->allocate(sizeof(value_t));
		void* storage = this->get_storage();
		new (storage) value_t(std::forward<Args>(args)...);
		any_ops_ = &type_operations<value_t>;
		return *static_cast<value_t*>(storage);
	}

	void swap(any_base& other)
		requires(Storage::can_always_swap || CopySupport > any_copy_support::no_copy_or_move)
	{
		auto move_into = [](any_base& dest, any_base& src) {
			dest.allocate(src.any_ops_->size());
			src.any_ops_->move(dest.get_storage(), src.get_storage());
			dest.any_ops_ = src.any_ops_;
			src.reset();
		};

		// Try the easy pointer swap first.
		if (this->try_swap(&other))
		{
			return;
		}

		// Handle the case if we don't have a value.
		if (!this->has_value())
		{
			if (!other.has_value())
			{
				// Neither has a value - we're done.
				return;
			}
			// The other has a value and we don't - just move from it.
			move_into(*this, other);
			return;
		}

		// Handle the case where only we have a value.
		if (!other.has_value())
		{
			move_into(other, *this);
			return;
		}

		// Both have values. We need a temporary.
		any_base temp;
		move_into(temp, *this);
		move_into(*this, other);
		move_into(other, temp);
	}

	void reset()
	{
		void* storage = this->get_storage();
		if (storage == nullptr)
		{
			return;
		}
		assert(any_ops_ != nullptr);
		any_ops_->destruct(storage);
		this->free();
		any_ops_ = nullptr;
	}

	bool has_value() const { return this->get_storage() != nullptr; }

	template <class T>
	bool has_type() const
	{
		return any_ops_ != nullptr && any_ops_->get_type_info() == get_type_info<T>();
	}

	template <class T>
	std::decay_t<T>& value()
	{
		assert(has_value());
		return *static_cast<std::decay_t<T>*>(this->get_storage());
	}

	template <class T>
	std::decay_t<T>* try_get_value()
	{
		return has_type<T>() ? static_cast<std::decay_t<T>*>(this->get_storage()) : nullptr;
	}

	template <class T>
	const std::decay_t<T>* try_get_value() const
	{
		return has_type<T>() ? static_cast<const std::decay_t<T>*>(this->get_storage()) : nullptr;
	}

private:
	template <any_storage OtherStorage, any_copy_support OtherCopySupport>
	void copy(const any_base<OtherStorage, OtherCopySupport>& other)
	{
		// Check to see if we should be copy-assigning.
		if (any_ops_ != nullptr && other.any_ops_ != nullptr &&
			any_ops_->get_type_info() == other.any_ops_->get_type_info())
		{
			any_ops_->copy_assign(this->get_storage(), other.get_storage());
			return;
		}

		reset();

		if (other.has_value())
		{
			this->allocate(other.any_ops_->size());
			other.any_ops_->copy(this->get_storage(), other.get_storage());
			any_ops_ = other.any_ops_;
		}
	}

	template <any_storage OtherStorage, any_copy_support OtherCopySupport>
	void move(any_base<OtherStorage, OtherCopySupport>& other)
	{
		// Check to see if we should be move-assigning.
		if (any_ops_ != nullptr && other.any_ops_ != nullptr &&
			any_ops_->get_type_info() == other.any_ops_->get_type_info())
		{
			any_ops_->move_assign(this->get_storage(), other.get_storage());
			return;
		}

		reset();

		if (other.has_value())
		{
			this->allocate(other.any_ops_->size());
			other.any_ops_->move(this->get_storage(), other.get_storage());
			any_ops_ = other.any_ops_;
			other.reset();
		}
	}

	const any_type_operations* any_ops_ = nullptr;
};

consteval std::false_type is_any(...);

template <any_storage Storage, any_copy_support CopySupport>
consteval std::true_type is_any(any_base<Storage, CopySupport>*);

} // namespace detail

template <any_copy_support CopySupport = any_copy_support::copy_and_move>
class heap_any : public detail::any_base<detail::any_heap_storage, CopySupport>
{
	using base_t = detail::any_base<detail::any_heap_storage, CopySupport>;

public:
	using base_t::base_t;
	using base_t::operator=;

	heap_any() = default;
	heap_any(const heap_any&) = default;
	heap_any& operator=(const heap_any&) = default;
	heap_any(heap_any&&) noexcept = default;
	heap_any& operator=(heap_any&&) noexcept = default;
};

template <size_t Size, any_copy_support CopySupport = any_copy_support::copy_and_move>
class any_of_size : public detail::any_base<detail::any_local_storage<Size>, CopySupport>
{
	using base_t = detail::any_base<detail::any_local_storage<Size>, CopySupport>;

public:
	any_of_size() = default;
	any_of_size(const any_of_size&) = default;
	any_of_size& operator=(const any_of_size&) = default;
	any_of_size(any_of_size&&) noexcept = default;
	any_of_size& operator=(any_of_size&&) noexcept = default;

	template <class T>
		requires(CopySupport == any_copy_support::copy_and_move &&
				 std::is_copy_constructible_v<T> && sizeof(T) <= Size)
	explicit any_of_size(const T& value)
	{
		emplace<T>(value);
	}

	template <class T>
		requires(CopySupport > any_copy_support::no_copy_or_move &&
				 std::is_move_constructible_v<T> && sizeof(T) <= Size)
	explicit any_of_size(T&& value)
	{
		emplace<T>(std::move(value));
	}

	template <class T>
		requires(CopySupport == any_copy_support::copy_and_move && std::is_copy_assignable_v<T> &&
				 sizeof(T) <= Size)
	any_of_size& operator=(const T& value)
	{
		base_t::operator=(value);
		return *this;
	}

	template <class T>
		requires(!std::is_lvalue_reference_v<T> &&
				 CopySupport > any_copy_support::no_copy_or_move && std::is_move_assignable_v<T> &&
				 sizeof(T) <= Size)
	any_of_size& operator=(T&& value) noexcept
	{
		base_t::operator=(std::move(value));
		return *this;
	}
};

template <any_copy_support CopySupport = any_copy_support::copy_and_move>
class any
	: public detail::any_base<detail::any_small_buffer_storage<2 * sizeof(void*) - 1>, CopySupport>
{
	using base_t =
		detail::any_base<detail::any_small_buffer_storage<2 * sizeof(void*) - 1>, CopySupport>;

public:
	using base_t::base_t;
	using base_t::operator=;

	any() = default;
	any(const any&) = default;
	any& operator=(const any&) = default;
	any(any&&) noexcept = default;
	any& operator=(any&&) noexcept = default;
};

static_assert(sizeof(any<>) == (3 * sizeof(void*)), "Internal error: any is not expected size");

template <class T>
concept any_any = std::is_same_v<std::true_type, decltype(detail::is_any(std::declval<T*>()))>;

static_assert(any_any<any<>>);

template <class T, any_any Any>
T* any_cast(Any* any) { return any->template try_get_value<T>(); }

template <class T, any_any Any>
const T* any_cast(const Any* any) { return any->template try_get_value<T>(); }

using copyable_any = any<any_copy_support::copy_and_move>;
using movable_any = any<any_copy_support::move_only>;
using nonmovable_any = any<any_copy_support::no_copy_or_move>;

} // namespace really
