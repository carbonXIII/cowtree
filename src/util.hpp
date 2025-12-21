#pragma once

#include <span>
#include <variant>
#include <stdexcept>
#include <ranges>

#define FORWARD(x) std::forward<decltype(x)>(x)

template <typename... T>
struct overloaded: T... { using T::operator()...; };

template <typename... T>
overloaded(T&&...) -> overloaded<T...>;

template <typename...>
struct cat_variant {};

template <typename... Ts, typename... Us>
struct cat_variant<std::variant<Ts...>, Us...> {
  using type = std::variant<Ts..., Us...>;
};

template <typename... T>
using cat_variant_t = cat_variant<T...>::type;

template <typename... T>
struct variant_cast_proxy {
  std::variant<T...> val;

  template <typename... U>
  operator std::variant<U...>() {
    return std::visit([](auto&& v) -> std::variant<U...> {
        return FORWARD(v);
    }, std::move(val));
  }
};

auto variant_cast(auto&& arg) { return variant_cast_proxy { std::move(arg) }; }

constexpr auto as_bytes(auto&& r) {
  return std::as_bytes(std::span{FORWARD(r)});
}

constexpr auto as_writable_bytes(auto&& r) {
  return std::as_writable_bytes(std::span{FORWARD(r)});
}

template <typename T, typename Storage>
struct block_view_proxy {
  union alignas(T) U {
    T t;
    std::array<std::byte, sizeof(T)> b;

    operator T&() & { return t; }
    operator T const&() const& { return t; }
  };

  [[no_unique_address]] Storage& s;

  constexpr T& operator[](size_t i) {
    return reinterpret_cast<U*>(&as_writable_bytes(s)[i * sizeof(U)])->t;
  }
};

template <typename T>
constexpr auto _block_view(auto& storage) {
  using StorageT = std::remove_pointer_t<std::ranges::range_value_t<decltype(storage)>>;

  union alignas(T) U {
    T t;
    std::array<std::byte, sizeof(T)> b;

    operator auto&() & { return t; }
    operator auto const&() const& { return t; }

    U* operator=(T const& t) {
      this->t = t;
      return this;
    }

    T* operator->() { return &t; }
    T& operator*() { return t; }
    T const* operator->() const { return &t; }
    T const& operator*() const { return t; }
  };

  if constexpr (std::is_const_v<StorageT>) {
    return std::span<U const> {
      reinterpret_cast<U const*>(storage.data()),
      storage.size() * sizeof(StorageT) / sizeof(T)
    };
  } else {
    return std::span<U> {
      reinterpret_cast<U*>(storage.data()),
      storage.size() * sizeof(StorageT) / sizeof(T)
    };
  }
}

template <typename T>
constexpr auto block_view(auto& storage) {
  return _block_view<T>(storage);
}

template <typename T>
constexpr auto block_view(auto const& storage) {
  return _block_view<T>(storage);
}

// FIXME: span_like constraint
auto copy(auto const& src, auto&& dst) {
  if(dst.size() < src.size()) throw std::out_of_range("dst.size() < src.size()");
  return std::ranges::copy(src, dst.begin());
}
