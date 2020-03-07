//
// JSONParsers.hpp
//
// Copyright © 2020 Ismo Kärkkäinen. All rights reserved.
//
// Licensed under Universal Permissive License. See License.txt.

#if !defined(JSONPARSERS_HPP)
#define JSONPARSERS_HPP

#include <exception>
#include <string>
#include <vector>
#include <tuple>
#include <utility>
#include <ctype.h>
#include <cstring>


class ParserException : public std::exception {
private:
    const char* reason;

public:
    ParserException(const char* Reason) : reason(Reason) { }
    const char* what() const throw() { return reason; }
};


class ParserPool;


class SimpleValueParser {
protected:
    bool finished;
    const char* setFinished(const char* Endptr, ParserPool& Pool);
    const char* setFinished(const char* Endptr);

public:
    SimpleValueParser() : finished(true) { }
    virtual ~SimpleValueParser();

    bool Finished() const { return finished; }
    // Returns nullptr if not finished (reached end) or pointer where ended.
    virtual const char* Scan(
        const char* Begin, const char* End, ParserPool& Pool)
        noexcept(false) = 0;
};


class ParseFloat : public SimpleValueParser {
public:
    typedef float Type;
    enum Pool { Index = 0 }; // Has to match ParserPool enum.

    const char* Scan(const char* Begin, const char* End, ParserPool& Pool)
        noexcept(false);
};


class ParseString : public SimpleValueParser {
private:
    int count;
    char hex_digits[4];
    bool escaped, began;

    bool scan(const char* Current, ParserPool& Pool) noexcept(false);

public:
    typedef std::string Type;
    enum Pool { Index = 1 }; // Has to match ParserPool enum.

    ParseString() : count(-1), escaped(false), began(false) { }
    const char* Scan(const char* Begin, const char* End, ParserPool& Pool)
        noexcept(false);
};


class SkipWhitespace : public SimpleValueParser {
public:
    typedef char Type;
    enum Pool { Index = 2 }; // Has to match ParserPool enum.

    const char* Scan(const char* Begin, const char* End, ParserPool& Pool)
        noexcept(false);
};

class ParserPool {
public:
    ParserPool() { }
    ParserPool(const ParserPool&) = delete;
    ParserPool& operator=(const ParserPool&) = delete;
    virtual ~ParserPool();

    enum Parsers { Float, String, Whitespace };
    std::vector<char> buffer;

    std::tuple<ParseFloat, ParseString, SkipWhitespace> Parser;
    std::tuple<ParseFloat::Type, ParseString::Type, SkipWhitespace::Type> Value;
};


extern ParserException NotFinished;

template<typename Parser>
class ParseArray : public SimpleValueParser {
public:
    typedef std::vector<typename Parser::Type> Type;

private:
    Type out;
    bool began, had_comma;

public:
    ParseArray() : began(false), had_comma(false) { }
    const char* Scan(const char* Begin, const char* End, ParserPool& Pool)
        noexcept(false);

    void Swap(Type& Alt) {
        if (!Finished())
            throw NotFinished;
        std::swap(Alt, out);
        out.resize(0);
    }
};


extern ParserException InvalidArrayStart;
extern ParserException InvalidArraySeparator;

template<typename Parser>
const char* ParseArray<Parser>::Scan(
    const char* Begin, const char* End, ParserPool& Pool) noexcept(false)
{
    Parser& p(std::get<Parser::Pool::Index>(Pool.Parser));
    auto& skipper(std::get<ParserPool::Whitespace>(Pool.Parser));
    typename Parser::Type& value(std::get<Parser::Pool::Index>(Pool.Value));
    if (!p.Finished()) {
        // In the middle of parsing value when buffer ended?
        Begin = p.Scan(Begin, End, Pool);
        if (Begin == nullptr)
            return setFinished(nullptr);
        out.push_back(value);
        had_comma = false;
    } else if (!began) {
        // Expect '[' on first call.
        if (*Begin != '[')
            throw InvalidArrayStart;
        began = true;
        had_comma = false;
        ++Begin;
    }
    while (Begin != End) {
        if (!out.empty() && !had_comma) {
            // Comma, maybe surrounded by spaces.
            if (*Begin == ',') // Most likely unless prettified.
                ++Begin;
            else {
                Begin = skipper.Scan(Begin, End, Pool);
                if (Begin == nullptr)
                    return setFinished(nullptr);
                if (*Begin == ']') {
                    began = false; // In case caller re-uses. Out must be empty.
                    return setFinished(++Begin);
                }
                if (*Begin != ',')
                    throw InvalidArraySeparator;
                Begin++;
            }
            had_comma = true;
        } else if (out.empty()) {
            Begin = skipper.Scan(Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            if (*Begin == ']') {
                began = false; // In case caller re-uses. Out must be empty.
                return setFinished(++Begin);
            }
        }
        Begin = skipper.Scan(Begin, End, Pool);
        if (Begin == nullptr)
            return setFinished(nullptr);
        // Now there should be the item to parse.
        Begin = p.Scan(Begin, End, Pool);
        if (Begin == nullptr)
            return setFinished(nullptr);
        out.push_back(value);
        had_comma = false;
    }
    return setFinished(nullptr);
}


template<typename Parser>
class ParseContainerArray : public SimpleValueParser {
public:
    typedef std::vector<typename Parser::Type> Type;

private:
    Parser p;
    Type out;
    bool began, had_comma;

public:
    ParseContainerArray() : began(false), had_comma(false) { }
    const char* Scan(const char* Begin, const char* End, ParserPool& Pool)
        noexcept(false);

    void Swap(Type& Alt) {
        if (!Finished())
            throw NotFinished;
        std::swap(Alt, out);
        out.resize(0);
    }
};


template<typename Parser>
const char* ParseContainerArray<Parser>::Scan(
    const char* Begin, const char* End, ParserPool& Pool) noexcept(false)
{
    auto& skipper(std::get<ParserPool::Whitespace>(Pool.Parser));
    if (!p.Finished()) { // In the middle of parsing value when buffer ended?
        Begin = p.Scan(Begin, End, Pool);
        if (Begin == nullptr)
            return setFinished(nullptr);
        out.push_back(typename Parser::Type());
        p.Swap(out.back());
        had_comma = false;
    } else if (!began) {
        // Expect '[' on first call.
        if (*Begin != '[')
            throw InvalidArrayStart;
        began = true;
        had_comma = false;
        ++Begin;
    }
    while (Begin != End) {
        if (!out.empty() && !had_comma) {
            // Comma, maybe surrounded by spaces.
            if (*Begin == ',') // Most likely unless prettified.
                ++Begin;
            else {
                Begin = skipper.Scan(Begin, End, Pool);
                if (Begin == nullptr)
                    return setFinished(nullptr);
                if (*Begin == ']') {
                    began = false; // In case caller re-uses. Out must be empty.
                    return setFinished(++Begin);
                }
                if (*Begin != ',')
                    throw InvalidArraySeparator;
                Begin++;
            }
            had_comma = true;
        } else if (out.empty()) {
            Begin = skipper.Scan(Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            if (*Begin == ']') {
                began = false; // In case caller re-uses. Out must be empty.
                return setFinished(++Begin);
            }
        }
        Begin = skipper.Scan(Begin, End, Pool);
        if (Begin == nullptr)
            return setFinished(nullptr);
        // Now there should be the item to parse.
        Begin = p.Scan(Begin, End, Pool);
        if (Begin == nullptr)
            return setFinished(nullptr);
        out.push_back(typename Parser::Type());
        p.Swap(out.back());
        had_comma = false;
    }
    return setFinished(nullptr);
}


class ValueStore;

// Helper class for implementation of KeyValues template.
class ScanningKeyValue {
protected:
    bool given;
public:
    ScanningKeyValue() : given(false) { }
    virtual ~ScanningKeyValue();
    virtual SimpleValueParser& Scanner(ParserPool& Pool) = 0;
    virtual const char* Key() const = 0;
    virtual void Swap(ValueStore* VS, ParserPool& Pool) = 0;
    virtual bool Required() const = 0;
    bool Given() const { return given; }
};

// Helper class for implementation of KeyValues template.
template<const char* KeyString, typename Parser>
class KeyValue : public ScanningKeyValue {
public:
    typedef typename Parser::Type Type;
    const char* Key() const { return KeyString; }
    void Swap(ValueStore* VS, ParserPool& Pool);

    void Swap(Type& Alt, ParserPool& Pool) {
        std::swap(Alt, std::get<Parser::Pool::Index>(Pool.Value));
        given = false;
    }

    SimpleValueParser& Scanner(ParserPool& Pool) {
        given = true;
        return std::get<Parser::Pool::Index>(Pool.Parser);
    }

    bool Required() const { return false; }
};

// Helper class for implementation of KeyValues template.
template<const char* KeyString, typename Parser>
class RequiredKeyValue : public KeyValue<KeyString,Parser> {
public:
    bool Required() const { return true; }
};

// Helper class for implementation of KeyValues template.
template<const char* KeyString, typename Parser>
class KeyContainerValue : public ScanningKeyValue {
private:
    Parser p;

public:
    typedef typename Parser::Type Type;
    const char* Key() const { return KeyString; }
    void Swap(ValueStore* VS, ParserPool& Pool);

    void Swap(Type& Alt, ParserPool& Pool) {
        p.Swap(Alt);
        given = false;
    }

    SimpleValueParser& Scanner(ParserPool& Pool) {
        given = true;
        return p;
    }

    bool Required() const { return false; }
};

// Helper class for implementation of KeyValues template.
template<const char* KeyString, typename Parser>
class RequiredKeyContainerValue : public KeyContainerValue<KeyString,Parser> {
    bool Required() const { return true; }
};


// Helper class for implementation of ParseObject class.
template<class ... Fields>
class KeyValues {
private:
    std::vector<ScanningKeyValue*> ptrs;

    template<typename FuncObj, typename Tuple, size_t... IdxSeq>
    void apply_to_each(FuncObj&& fo, Tuple&& t, std::index_sequence<IdxSeq...>)
    {
        int a[] = {
            (fo(std::get<IdxSeq>(std::forward<Tuple>(t))), void(), 0) ...
        };
        (void)a; // Suppresses unused variable warning.
    }

    class Pusher {
    private:
        std::vector<ScanningKeyValue*>& tgt;
    public:
        Pusher(std::vector<ScanningKeyValue*>& Target) : tgt(Target) { }
        void operator()(ScanningKeyValue& SKV) { tgt.push_back(&SKV); }
    };

public:
    std::tuple<Fields...> fields;

    KeyValues() {
        Pusher psh(ptrs);
        apply_to_each(psh, fields,
            std::make_index_sequence<std::tuple_size<decltype(fields)>::value>
                {});
    }

    size_t size() const { return std::tuple_size<decltype(fields)>::value; }
    SimpleValueParser& Scanner(size_t Index, ParserPool& Pool) {
        return ptrs[Index]->Scanner(Pool);
    }
    ScanningKeyValue* KeyValue(size_t Index) { return ptrs[Index]; }
};

// Another derived class adds all the convenience methods that map to index.

class ValueStore {
protected:
    bool given;
public:
    ValueStore() : given(false) { }
    virtual ~ValueStore();
    void Give() { given = true; }
    bool Given() const { return given; }
};

template<typename Parser>
class Value : public ValueStore {
public:
    typedef typename Parser::Type Type;
    Type value;
};

/*
template<typename Parser>
class RequiredContainerValue : public ValueStore {
public:
    typedef typename Parser::Type Type;
    Type value;
    void Give() { }
    bool Given() const { return true; }
};

template<typename Parser>
class ContainerValue : public RequiredContainerValue<Parser> {
private:
    bool given;
public:
    ContainerValue() : given(false) { }
    void Give() { given = true; }
    bool Given() const { return given; }
};
*/

template<class ... Fields>
class NamelessValues {
private:
    std::vector<ValueStore*> ptrs;

    template<typename FuncObj, typename Tuple, size_t... IdxSeq>
    void apply_to_each(FuncObj&& fo, Tuple&& t, std::index_sequence<IdxSeq...>)
    {
        int a[] = {
            (fo(std::get<IdxSeq>(std::forward<Tuple>(t))), void(), 0) ...
        };
        (void)a; // Suppresses unused variable warning.
    }

    class Pusher {
    private:
        std::vector<ValueStore*>& tgt;
    public:
        Pusher(std::vector<ValueStore*>& Target) : tgt(Target) { }
        void operator()(ValueStore& VS) { tgt.push_back(&VS); }
    };

public:
    std::tuple<Fields...> fields;

    NamelessValues() {
        Pusher psh(ptrs);
        apply_to_each(psh, fields, std::make_index_sequence<std::tuple_size<decltype(fields)>::value> {});
    }

    size_t size() const { return std::tuple_size<decltype(fields)>::value; }
    ValueStore* operator[](size_t Index) { return ptrs[Index]; }
};

template<const char* KeyString, typename Parser>
void KeyValue<KeyString,Parser>::Swap(ValueStore* VS, ParserPool& Pool) {
    Value<Parser>* dst(dynamic_cast<Value<Parser>*>(VS));
    std::swap(dst->value, std::get<Parser::Pool::Index>(Pool.Value));
    VS->Give();
    given = false;
}

template<const char* KeyString, typename Parser>
void KeyContainerValue<KeyString,Parser>::Swap(ValueStore* VS, ParserPool& Pool)
{
    Value<Parser>* dst(dynamic_cast<Value<Parser>*>(VS));
    p.Swap(dst->value);
    VS->Give();
    given = false;
}

// The Values class used by the template should be derived from NamelessValues
// and it adds way to access the fields using sensibly named methods.

extern ParserException InvalidKey;
extern ParserException RequiredKeyNotGiven;

template<typename KeyValues, typename Values>
class ParseObject : public SimpleValueParser {
private:
    KeyValues parsers;
    Values out;
    int activating, active;
    enum State {
        NotStarted,
        PreKey,
        ExpectKey,
        PreColon,
        ExpectColon,
        PreValue,
        ExpectValue,
        PreComma,
        ExpectComma
    };
    State state;

    void setActivating(const std::string& Incoming) {
        for (int k = 0; k < parsers.size(); ++k)
            if (strcmp(Incoming.c_str(), parsers.KeyValue(k)->Key()) == 0) {
                activating = k;
                return;
            }
        throw InvalidKey;
    }

    const char* checkPassed(const char* Ptr) noexcept(false) {
        for (size_t k = 0; k < out.size(); ++k) {
            ScanningKeyValue* skv = parsers.KeyValue(k);
            ValueStore* vs = out[k];
            if (skv->Required() && !vs->Given())
                throw RequiredKeyNotGiven;
        }
        state = NotStarted;
        activating = -1;
        active = -1;
        return setFinished(Ptr);
    }

public:
    typedef decltype(out.fields) Type;

    ParseObject() : activating(-1), active(-1), state(NotStarted) { }

    const char* Scan(const char* Begin, const char* End, ParserPool& Pool)
        noexcept(false);

    void Swap(Type& Alt) {
        std::swap(Alt, out.fields);
        out.fields = Type();
    }
};

extern ParserException InvalidObjectStart;
extern ParserException InvalidKeySeparator;
extern ParserException InvalidValueSeparator;

template<typename KeyValues, typename Values>
const char* ParseObject<KeyValues,Values>::Scan(
    const char* Begin, const char* End, ParserPool& Pool) noexcept(false)
{
    if (state == NotStarted) {
        // Expect '{' on the first call.
        if (*Begin != '{')
            throw InvalidObjectStart;
        state = PreKey;
        ++Begin;
    }
    auto& skipper(std::get<ParserPool::Whitespace>(Pool.Parser));
    while (Begin != End) {
        // Re-order states to most expected first once works.
        if (state == PreKey) {
            Begin = skipper.Scan(Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            if (*Begin == '}')
                return checkPassed(++Begin);
            state = ExpectKey;
        }
        if (state == ExpectKey) {
            Begin = std::get<ParserPool::String>(Pool.Parser).Scan(
                Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            setActivating(std::get<ParserPool::String>(Pool.Value));
            state = PreColon;
        }
        if (state == PreColon) {
            Begin = skipper.Scan(Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            state = ExpectColon;
        }
        if (state == ExpectColon) {
            if (*Begin != ':')
                throw InvalidKeySeparator;
            state = PreValue;
            if (++Begin == End)
                return setFinished(nullptr);
        }
        if (state == PreValue) {
            Begin = skipper.Scan(Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            active = activating;
            activating = -1;
            state = ExpectValue;
        }
        if (state == ExpectValue) {
            Begin = parsers.Scanner(active, Pool).Scan(Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            parsers.KeyValue(active)->Swap(out[active], Pool);
            active = -1;
            state = PreComma;
        }
        if (state == PreComma) {
            Begin = skipper.Scan(Begin, End, Pool);
            if (Begin == nullptr)
                return setFinished(nullptr);
            state = ExpectComma;
        }
        if (state == ExpectComma) {
            if (*Begin == '}')
                return checkPassed(++Begin);
            if (*Begin != ',')
                throw InvalidValueSeparator;
            state = PreKey;
            ++Begin;
        }
    }
    return setFinished(nullptr);
}

// Something reading stdin or file and this receiving blocks and freeing after
// they have been iterated past is another thing.
// Separate reader and iterator. Iterator gets fed more data and has end
// indicated. Reader does the feeding. Iterator is container with iterator-like
// methods. In fact storage and reader base classes in one.
// Have pointer to last item in contiguous memory block be available and
// byte after that must be zero. Then we can scan without copy to buffer,
// copying only when item spans memory blocks.
// Gives plenty of opportunities to check how it'll work.
// Null terminator is required to convert a number directly. Does C allow
// representation that JSON does not? If so, would have to scan to verify.
// String has to be scanned.

// Assuming we get data from something, and the block allocator places null
// terminator as appropriate for parser and uses reader to fill up data
// before that, we can pass start-end pairs to parser and whenever the
// last item may continue outside block end, parse takes a local copy of
// it as appropriate. FOr number just the number part (short usually) and
// for string it accumulates a copy for std::string anyway.

// multi-dimensional array of one type would probably be more
// efficient. One just needs to have operator() set properly and scalar type.

// the array, array, array could be cube and array, array a plane for short
// it would also tell if all sub-arrays are expected to be of same length in
// the same direction.

// That would simplify specifying what you have but needs explaining
// Then again array, array, array is for case where they are of different length
// The limitation needs to be specified anyway somehow.
// Maybe take limitation and format and combine them?

#endif