#pragma once

#include <cxxabi.h>

#include <memory>
#include <string>
#include <typeinfo>

std::string DemangeTypeName(const char* name);

template <class T>
std::string TypeToString();

template <class T>
std::string TypeToString(const T& t);

template <class T>
std::string TypeToString(const T* t);

inline std::string DemangeTypeName(const char* name) {
    int status = -4;  // some arbitrary value to eliminate the compiler warning

    // enable c++11 by passing the flag -std=c++11 to g++
    std::unique_ptr<char, void (*)(void*)> res{abi::__cxa_demangle(name, NULL, NULL, &status), std::free};

    return (status == 0) ? res.get() : name;
}

template <class T>
std::string TypeToString() {
    return DemangeTypeName(typeid(T).name());
}

template <class T>
std::string TypeToString(const T& t) {
    return DemangeTypeName(typeid(t).name());
}

template <class T>
std::string TypeToString(const T* t) {
    return DemangeTypeName(typeid(t).name());
}