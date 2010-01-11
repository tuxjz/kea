// Copyright (C) 2010  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

// $Id$

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <string>
#include <sstream>
#include <utility>

#include <stdint.h>

#include <boost/shared_ptr.hpp>

#include "exceptions.h"
#include "rrparamregistry.h"
#include "rrclass.h"
#include "rrtype.h"

using namespace std;
using namespace boost;

namespace isc {
namespace dns {

namespace {
bool CICharEqual(char c1, char c2)
{
    return (tolower(static_cast<unsigned char>(c1)) ==
            tolower(static_cast<unsigned char>(c2)));
}

bool
caseStringEqual(const string& s1, const string& s2, size_t n)
{
    if (s1.size() < n || s2.size() < n) {
        return (false);
    }

    return (mismatch(s1.begin(), s1.begin() + n, s2.begin(), CICharEqual).first
            == s1.begin() + n);
}

bool
CICharLess(char c1, char c2)
{
    return (tolower(static_cast<unsigned char>(c1)) <
            tolower(static_cast<unsigned char>(c2)));
}

struct CIStringLess :
        public binary_function<string, string, bool>
{
    bool operator()(const string& s1, const string& s2) const
    {
        return (lexicographical_compare(s1.begin(), s1.end(),
                                        s2.begin(), s2.end(), CICharLess));
    }
};

struct RRTypeParam {
    RRTypeParam(const string& code_string, uint16_t code) :
        code_string_(code_string), code_(code) {}
    string code_string_;
    uint16_t code_;

    /// magic constants
    static const unsigned int MAX_CODE = 0xffff;
    static const string UNKNOWN_PREFIX;
    static const size_t UNKNOWN_PREFIXLEN;
    static const string UNKNOWN_MAX;
    static const size_t UNKNOWN_MAXLEN;
};

typedef shared_ptr<RRTypeParam> RRTypeParamPtr;
typedef map<string, RRTypeParamPtr, CIStringLess> StrRRTypeMap;
typedef map<uint16_t, RRTypeParamPtr> CodeRRTypeMap;

const string RRTypeParam::UNKNOWN_PREFIX = "TYPE";
const size_t RRTypeParam::UNKNOWN_PREFIXLEN =
    RRTypeParam::UNKNOWN_PREFIX.size();
const string RRTypeParam::UNKNOWN_MAX = "TYPE65535";
const size_t RRTypeParam::UNKNOWN_MAXLEN =
    RRTypeParam::UNKNOWN_MAX.size();

struct RRClassParam {
    RRClassParam(const string& code_string, uint16_t code) :
        code_string_(code_string), code_(code) {}
    string code_string_;
    uint16_t code_;

    /// magic constants
    static const unsigned int MAX_CODE = 0xffff;
    static const string UNKNOWN_PREFIX;
    static const size_t UNKNOWN_PREFIXLEN;
    static const string UNKNOWN_MAX;
    static const size_t UNKNOWN_MAXLEN;
};

typedef shared_ptr<RRClassParam> RRClassParamPtr;
typedef map<string, RRClassParamPtr, CIStringLess> StrRRClassMap;
typedef map<uint16_t, RRClassParamPtr> CodeRRClassMap;

const string RRClassParam::UNKNOWN_PREFIX = "CLASS";
const size_t RRClassParam::UNKNOWN_PREFIXLEN =
    RRClassParam::UNKNOWN_PREFIX.size();
const string RRClassParam::UNKNOWN_MAX = "CLASS65535";
const size_t RRClassParam::UNKNOWN_MAXLEN =
    RRClassParam::UNKNOWN_MAX.size();
}

struct RRParamRegistryImpl {
    StrRRClassMap str2classmap;
    CodeRRClassMap code2classmap;
    StrRRTypeMap str2typemap;
    CodeRRTypeMap code2typemap;
};

RRParamRegistry::RRParamRegistry()
{
    impl_ = new RRParamRegistryImpl;

    // set up parameters for well-known RRs
    // XXX: this should eventually be more automatic.
    try {
        add("IN", 1, "A", 1);
        add("IN", 1, "NS", 2);

        add("CH", 3, "A", 1);
    } catch (...) {
        delete impl_;
        throw;
    }
    //...
}

RRParamRegistry::~RRParamRegistry()
{
    delete impl_;
}

const RRParamRegistry&
RRParamRegistry::getRegistry()
{
    static RRParamRegistry registry;

    return (registry);
}

void
RRParamRegistry::add(const string& classcode_string, uint16_t classcode,
                     const string& typecode_string, uint16_t typecode
                     /* rdata_factory (notyet) */)
{
    // XXX: rollback logic on failure is complicated.
    bool add_type = false;
    bool add_class = false;

    if (impl_->code2typemap.find(typecode) == impl_->code2typemap.end()) {
        add_type = true;
    }
    if (impl_->code2classmap.find(classcode) == impl_->code2classmap.end()) {
        add_class = true;
    }

    try {
        addType(typecode_string, typecode);
        addClass(classcode_string, classcode);
    } catch (...) {
        if (add_type) {
            removeType(typecode);
        }
        if (add_class) {
            removeClass(classcode);
        }
        throw;
    }
}

namespace {
/// Code logic for RRTypes and RRClasses is mostly common except (C++) type and
/// member names.  So we define type-independent templates to describe the
/// common logic and let concrete classes to avoid code duplicates.
/// The following summarize template parameters used in the set of template
/// functions:
/// PT: parameter type, either RRTypeParam or RRClassParam
/// MC: type of mapping class from code: either CodeRRTypeMap or CodeRRClassMap
/// MS: type of mapping class from string: either StrRRTypeMap or StrRRClassMap
/// ET: exception type for error handling: either InvalidRRType or
///     InvalidRRClass
template <typename PT, typename MC, typename MS>
inline void
addParam(const string& code_string, uint16_t code, MC& codemap, MS& stringmap)
{
    // Duplicate type check
    typename MC::const_iterator found = codemap.find(code);
    if (codemap.find(code) != codemap.end()) {
        if (codemap.find(code)->second->code_string_ != code_string) {
            dns_throw(RRClassExist, "Duplicate RR parameter registration");
        }
        return;
    }

    typedef shared_ptr<PT> ParamPtr;
    typedef pair<string, ParamPtr> StrParamPair;
    typedef pair<uint16_t, ParamPtr> CodeParamPair;
    ParamPtr param = ParamPtr(new PT(code_string, code));
    try {
        stringmap.insert(StrParamPair(code_string, param));
        codemap.insert(CodeParamPair(code, param));
    } catch (...) {
        // Rollback to the previous state: not all of the erase operations will
        // find the entry, but we don't care.
        stringmap.erase(code_string);
        codemap.erase(code);
        throw;
    }
}

template <typename MC, typename MS>
inline bool
removeParam(uint16_t code, MC& codemap, MS& stringmap)
{
    typename MC::iterator found = codemap.find(code);

    if (found != codemap.end()) {
        size_t erased = stringmap.erase(found->second->code_string_);
        // We must have a corresponding entry of the str2 map exists
        assert(erased == 1);

        codemap.erase(found);

        return (true);
    }

    return (false);
}

template <typename PT, typename MS, typename ET>
inline uint16_t
getCode(const string& code_str, MS& stringmap)
{
    typename MS::const_iterator found;

    found = stringmap.find(code_str);
    if (found != stringmap.end()) {
        return (found->second->code_);
    }

    size_t l = code_str.size();
    if (l > PT::UNKNOWN_PREFIXLEN &&
        l <= PT::UNKNOWN_MAXLEN &&
        caseStringEqual(code_str, PT::UNKNOWN_PREFIX, PT::UNKNOWN_PREFIXLEN)) {
        unsigned int code;
        istringstream iss(code_str.substr(PT::UNKNOWN_PREFIXLEN,
                                           l - PT::UNKNOWN_PREFIXLEN));
        iss >> dec >> code;
        if (iss.rdstate() == ios::eofbit && code <= PT::MAX_CODE) {
            return (code);
        }
    }
    dns_throw(ET, "Unrecognized RR parameter string");
}

template <typename PT, typename MC>
inline string
getText(uint16_t code, MC& codemap)
{
    typename MC::const_iterator found;

    found = codemap.find(code);
    if (found != codemap.end()) {
        return (found->second->code_string_);
    }

    ostringstream ss;
    ss << code;
    return (PT::UNKNOWN_PREFIX + ss.str());
}
}

void
RRParamRegistry::addType(const string& type_string, uint16_t code)
{
    addParam<RRTypeParam, CodeRRTypeMap, StrRRTypeMap>(type_string, code,
                                                       impl_->code2typemap,
                                                       impl_->str2typemap);
}

bool
RRParamRegistry::removeType(uint16_t code)
{
    return (removeParam<CodeRRTypeMap, StrRRTypeMap>(code, impl_->code2typemap,
                                                     impl_->str2typemap));
}

uint16_t
RRParamRegistry::getTypeCode(const string& type_str) const
{
    return (getCode<RRTypeParam, StrRRTypeMap,
            InvalidRRType>(type_str, impl_->str2typemap));
}

string
RRParamRegistry::getTypeText(uint16_t code) const
{
    return (getText<RRTypeParam, CodeRRTypeMap>(code, impl_->code2typemap));
}

void
RRParamRegistry::addClass(const string& class_string, uint16_t code)
{
    addParam<RRClassParam, CodeRRClassMap, StrRRClassMap>(class_string, code,
                                                          impl_->code2classmap,
                                                          impl_->str2classmap);
}

bool
RRParamRegistry::removeClass(uint16_t code)
{
    return (removeParam<CodeRRClassMap, StrRRClassMap>(code,
                                                       impl_->code2classmap,
                                                       impl_->str2classmap));
}

uint16_t
RRParamRegistry::getClassCode(const string& class_str) const
{
    return (getCode<RRClassParam, StrRRClassMap,
            InvalidRRClass>(class_str, impl_->str2classmap));
}

string
RRParamRegistry::getClassText(uint16_t code) const
{
    return (getText<RRClassParam, CodeRRClassMap>(code, impl_->code2classmap));
}
}
}
