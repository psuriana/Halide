#ifndef MLVAL_H
#define MLVAL_H

#include <memory>
#include <stdint.h>

using std::shared_ptr;

class MLVal {
public:
    static MLVal find(const char *name);
    MLVal() {}
    MLVal(const char *);
    MLVal(const std::string &);
    MLVal(int);
    MLVal(uint32_t);
    MLVal(float);
    MLVal(double);
    MLVal(void *);
    MLVal operator()() const;
    MLVal operator()(const MLVal &a) const;
    MLVal operator()(const MLVal &a, const MLVal &b) const;
    MLVal operator()(const MLVal &a, const MLVal &b, const MLVal &c) const;
    MLVal operator()(const MLVal &a, const MLVal &b, const MLVal &c, const MLVal &d) const;
    MLVal operator()(const MLVal &a, const MLVal &b, const MLVal &c, const MLVal &d, const MLVal &e) const;
    MLVal operator()(const MLVal &a, const MLVal &b, const MLVal &c, const MLVal &d, const MLVal &e, const MLVal &f) const;

    operator bool() const {return contents.get();}

    void *asVoidPtr() const;

    static MLVal fromString(const char *);
    static MLVal fromString(const std::string &);
    static MLVal fromInt(int);
    static MLVal fromFloat(float);
    static MLVal fromPointer(void *);

    static void unpackPair(const MLVal &input, MLVal &first, MLVal &second);
 private:
    struct Contents;
    shared_ptr<Contents> contents;
};

// Macro to help dig out functions
#define ML_FUNC0(n)                                            \
    MLVal n() {                                                \
        static MLVal callback;                                 \
        if (!callback) callback = MLVal::find(#n);             \
        return callback();                                     \
    }

#define ML_FUNC1(n)                                            \
    MLVal n(const MLVal &x) {                                         \
        static MLVal callback;                                 \
        if (!callback) callback = MLVal::find(#n);             \
        return callback(x);                                    \
    }

#define ML_FUNC2(n)                                                    \
    MLVal n(const MLVal &x, const MLVal &y) {                                        \
        static MLVal callback;                                         \
        if (!callback) callback = MLVal::find(#n);                     \
        return callback(x, y);                                         \
    }

#define ML_FUNC3(n)                                                    \
    MLVal n(const MLVal &x, const MLVal &y, const MLVal &z) {                               \
        static MLVal callback;                                         \
        if (!callback) callback = MLVal::find(#n);                     \
        return callback(x, y, z);                                      \
    }


#define ML_FUNC4(n)                                                    \
    MLVal n(const MLVal &x, const MLVal &y, const MLVal &z, const MLVal &w) {                      \
        static MLVal callback;                                         \
        if (!callback) callback = MLVal::find(#n);                     \
        return callback(x, y, z, w);                                   \
    }


#define ML_FUNC5(n)                                                    \
    MLVal n(const MLVal &a, const MLVal &b, const MLVal &c, const MLVal &d, const MLVal &e) {             \
        static MLVal callback;                                         \
        if (!callback) callback = MLVal::find(#n);                     \
        return callback(a, b, c, d, e);                                \
    }

#define ML_FUNC6(n)                                                    \
    MLVal n(const MLVal &a, const MLVal &b, const MLVal &c, const MLVal &d, const MLVal &e, const MLVal &f) {    \
        static MLVal callback;                                         \
        if (!callback) callback = MLVal::find(#n);                     \
        return callback(a, b, c, d, e, f);                             \
    }
 
#endif
