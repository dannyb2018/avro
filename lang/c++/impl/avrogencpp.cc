/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <cctype>
#ifndef _WIN32
#include <ctime>
#endif
#include <fstream>
#include <iostream>
#include <locale>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <utility>

#include "Compiler.hh"
#include "NodeImpl.hh"
#include "ValidSchema.hh"

using avro::NodePtr;
using avro::resolveSymbol;
using std::ifstream;
using std::map;
using std::ofstream;
using std::ostream;
using std::set;
using std::string;
using std::vector;

using avro::compileJsonSchema;
using avro::ValidSchema;

struct PendingSetterGetter {
    string structName;
    string type;
    string name;
    size_t idx;

    PendingSetterGetter(string sn, string t, string n, size_t i) : structName(std::move(sn)), type(std::move(t)), name(std::move(n)), idx(i) {}
};

struct PendingConstructor {
    string structName;
    string memberName;
    bool initMember;
    PendingConstructor(string sn, string n, bool im) : structName(std::move(sn)), memberName(std::move(n)), initMember(im) {}
};

class UnionCodeTracker {
    std::string schemaFile_;
    size_t unionNumber_ = 0;
    std::map<std::vector<std::string>, std::string> unionBranchNameMapping_;
    std::set<std::string> generatedUnionTraits_;

public:
    explicit UnionCodeTracker(const std::string &schemaFile);
    std::optional<std::string> getExistingUnionName(const std::vector<std::string> &unionBranches) const;
    std::string generateNewUnionName(const std::vector<std::string> &unionBranches);
    bool unionTraitsAlreadyGenerated(const std::string &unionClassName) const;
    void setTraitsGenerated(const std::string &unionClassName);
};

class CodeGen {
    UnionCodeTracker unionTracker_;
    std::ostream &os_;
    bool inNamespace_;
    const std::string ns_;
    const std::string schemaFile_;
    const std::string headerFile_;
    const std::string includePrefix_;
    const bool noUnion_;
    const std::string guardString_;
    std::mt19937 random_;

    vector<PendingSetterGetter> pendingGettersAndSetters;
    vector<PendingConstructor> pendingConstructors;

    map<NodePtr, string> done;
    set<NodePtr> doing;

    std::string guard();
    std::string fullname(const string &name) const;
    std::string generateEnumType(const NodePtr &n);
    std::string cppTypeOf(const NodePtr &n);
    std::string generateRecordType(const NodePtr &n);
    std::string generateUnionType(const NodePtr &n);
    std::string generateType(const NodePtr &n);
    std::string generateDeclaration(const NodePtr &n);
    std::string doGenerateType(const NodePtr &n);
    void generateEnumTraits(const NodePtr &n);
    void generateTraits(const NodePtr &n);
    void generateRecordTraits(const NodePtr &n);
    void generateUnionTraits(const NodePtr &n);
    void generateDocComment(const NodePtr &n, const char *indent = "");
    void emitCopyright();
    void emitGeneratedWarning();

public:
    CodeGen(std::ostream &os, std::string ns,
            std::string schemaFile, std::string headerFile,
            std::string guardString,
            std::string includePrefix, bool noUnion) : unionTracker_(schemaFile), os_(os), inNamespace_(false), ns_(std::move(ns)),
                                                       schemaFile_(std::move(schemaFile)), headerFile_(std::move(headerFile)),
                                                       includePrefix_(std::move(includePrefix)), noUnion_(noUnion),
                                                       guardString_(std::move(guardString)),
                                                       random_(static_cast<uint32_t>(::time(nullptr))) {
    }

    void generate(const ValidSchema &schema);
};

static string decorate(const std::string &name) {
    static const char *cppReservedWords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
        "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
        "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
        "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else",
        "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "import", "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept", "not",
        "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "reflexpr",
        "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "synchronized", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};

    for (auto &cppReservedWord : cppReservedWords)
        if (strcmp(name.c_str(), cppReservedWord) == 0)
            return name + '_';
    return name;
}

static string decorate(const avro::Name &name) {
    return decorate(name.simpleName());
}

string CodeGen::fullname(const string &name) const {
    return ns_.empty() ? name : (ns_ + "::" + name);
}

string CodeGen::generateEnumType(const NodePtr &n) {
    string s = decorate(n->name());
    os_ << "enum class " << s << ": unsigned {\n";
    size_t c = n->names();
    for (size_t i = 0; i < c; ++i) {
        os_ << "    " << decorate(n->nameAt(i)) << ",\n";
    }
    os_ << "};\n\n";
    return s;
}

string CodeGen::cppTypeOf(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_STRING:
            return "std::string";
        case avro::AVRO_BYTES:
            return "std::vector<uint8_t>";
        case avro::AVRO_INT:
            return "int32_t";
        case avro::AVRO_LONG:
            return "int64_t";
        case avro::AVRO_FLOAT:
            return "float";
        case avro::AVRO_DOUBLE:
            return "double";
        case avro::AVRO_BOOL:
            return "bool";
        case avro::AVRO_RECORD:
        case avro::AVRO_ENUM: {
            string nm = decorate(n->name());
            return inNamespace_ ? nm : fullname(nm);
        }
        case avro::AVRO_ARRAY:
            return "std::vector<" + cppTypeOf(n->leafAt(0)) + " >";
        case avro::AVRO_MAP:
            return "std::map<std::string, " + cppTypeOf(n->leafAt(1)) + " >";
        case avro::AVRO_FIXED:
            return "std::array<uint8_t, " + std::to_string(n->fixedSize()) + ">";
        case avro::AVRO_SYMBOLIC:
            return cppTypeOf(resolveSymbol(n));
        case avro::AVRO_UNION:
            return fullname(done[n]);
        case avro::AVRO_NULL:
            return "avro::null";
        default:
            return "$Undefined$";
    }
}

static string cppNameOf(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_NULL:
            return "null";
        case avro::AVRO_STRING:
            return "string";
        case avro::AVRO_BYTES:
            return "bytes";
        case avro::AVRO_INT:
            return "int";
        case avro::AVRO_LONG:
            return "long";
        case avro::AVRO_FLOAT:
            return "float";
        case avro::AVRO_DOUBLE:
            return "double";
        case avro::AVRO_BOOL:
            return "bool";
        case avro::AVRO_RECORD:
        case avro::AVRO_ENUM:
        case avro::AVRO_FIXED:
            return decorate(n->name());
        case avro::AVRO_ARRAY:
            return "array";
        case avro::AVRO_MAP:
            return "map";
        case avro::AVRO_SYMBOLIC:
            return cppNameOf(resolveSymbol(n));
        default:
            return "$Undefined$";
    }
}

string CodeGen::generateRecordType(const NodePtr &n) {
    size_t c = n->leaves();
    string decoratedName = decorate(n->name());
    vector<string> types;
    for (size_t i = 0; i < c; ++i) {
        types.push_back(generateType(n->leafAt(i)));
    }

    map<NodePtr, string>::const_iterator it = done.find(n);
    if (it != done.end()) {
        return it->second;
    }

    generateDocComment(n);
    os_ << "struct " << decoratedName << " {\n";
    if (!noUnion_) {
        for (size_t i = 0; i < c; ++i) {
            if (n->leafAt(i)->type() == avro::AVRO_UNION) {
                os_ << "    typedef " << types[i]
                    << ' ' << n->nameAt(i) << "_t;\n";
                types[i] = n->nameAt(i) + "_t";
            }
            if (n->leafAt(i)->type() == avro::AVRO_ARRAY && n->leafAt(i)->leafAt(0)->type() == avro::AVRO_UNION) {
                os_ << "    typedef " << types[i] << "::value_type"
                    << ' ' << n->nameAt(i) << "_item_t;\n";
            }
        }
    }
    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        generateDocComment(n->leafAt(i), "    ");
        os_ << "    " << types[i];
        os_ << ' ' << decoratedNameAt << ";\n";
    }

    os_ << "    " << decoratedName << "()";
    if (c > 0) {
        os_ << " :";
    }
    os_ << "\n";
    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "        " << decoratedNameAt << "(";
        os_ << types[i];
        os_ << "())";
        if (i != (c - 1)) {
            os_ << ',';
        }
        os_ << "\n";
    }
    os_ << "        { }\n";
    os_ << "};\n\n";
    return decoratedName;
}

void makeCanonical(string &s, bool foldCase) {
    for (char &c : s) {
        if (isalpha(c)) {
            if (foldCase) {
                c = static_cast<char>(toupper(c));
            }
        } else if (!isdigit(c)) {
            c = '_';
        }
    }
}

static void generateGetterAndSetter(ostream &os,
                                    const string &structName, const string &type, const string &name,
                                    size_t idx) {
    string sn = " " + structName + "::";

    os << "inline\n";

    os << "const " << type << "&" << sn << "get_" << name << "() const {\n"
       << "    if (idx_ != " << idx << ") {\n"
       << "        throw avro::Exception(\"Invalid type for "
       << "union " << structName << "\");\n"
       << "    }\n"
       << "    return *std::any_cast<" << type << " >(&value_);\n"
       << "}\n\n";

    os << "inline\n"
       << type << "&" << sn << "get_" << name << "() {\n"
       << "    if (idx_ != " << idx << ") {\n"
       << "        throw avro::Exception(\"Invalid type for "
       << "union " << structName << "\");\n"
       << "    }\n"
       << "    return *std::any_cast<" << type << " >(&value_);\n"
       << "}\n\n";

    os << "inline\n"
       << "void" << sn << "set_" << name
       << "(const " << type << "& v) {\n"
       << "    idx_ = " << idx << ";\n"
       << "    value_ = v;\n"
       << "}\n\n";

    os << "inline\n"
       << "void" << sn << "set_" << name
       << "(" << type << "&& v) {\n"
       << "    idx_ = " << idx << ";\n"
       << "    value_ = std::move(v);\n"
       << "}\n\n";
}

static void generateConstructor(ostream &os,
                                const string &structName, bool initMember,
                                const string &type) {
    os << "inline " << structName << "::" << structName << "() : idx_(0)";
    if (initMember) {
        os << ", value_(" << type << "())";
    }
    os << " { }\n";
}

/**
 * Generates a type for union and emits the code.
 * Since unions can encounter names that are not fully defined yet,
 * such names must be declared and the inline functions deferred until all
 * types are fully defined.
 */
string CodeGen::generateUnionType(const NodePtr &n) {
    size_t c = n->leaves();
    vector<string> types;
    vector<string> names;

    auto it = doing.find(n);
    if (it != doing.end()) {
        for (size_t i = 0; i < c; ++i) {
            const NodePtr &nn = n->leafAt(i);
            types.push_back(generateDeclaration(nn));
            names.push_back(cppNameOf(nn));
        }
    } else {
        doing.insert(n);
        for (size_t i = 0; i < c; ++i) {
            const NodePtr &nn = n->leafAt(i);
            types.push_back(generateType(nn));
            names.push_back(cppNameOf(nn));
        }
        doing.erase(n);
    }
    if (done.find(n) != done.end()) {
        return done[n];
    }

    // re-use existing union types that have the exact same branches
    if (const auto existingName = unionTracker_.getExistingUnionName(types); existingName.has_value()) {
        return existingName.value();
    }
    const std::string result = unionTracker_.generateNewUnionName(types);

    os_ << "struct " << result << " {\n"
        << "private:\n"
        << "    size_t idx_;\n"
        << "    std::any value_;\n"
        << "public:\n";

    os_ << "    /** enum representing union branches as returned by the idx() function */\n"
        << "    enum class Branch: size_t {\n";

    // generate a enum that maps the branch name to the corresponding index (as returned by idx())
    std::set<std::string> used_branch_names;
    for (size_t i = 0; i < c; ++i) {
        // escape reserved literals for c++
        auto branch_name = decorate(names[i]);
        // avoid rare collisions, e.g. someone might name their struct int_
        if (used_branch_names.find(branch_name) != used_branch_names.end()) {
            size_t postfix = 2;
            std::string escaped_name = branch_name + "_" + std::to_string(postfix);
            while (used_branch_names.find(escaped_name) != used_branch_names.end()) {
                ++postfix;
                escaped_name = branch_name + "_" + std::to_string(postfix);
            }
            branch_name = escaped_name;
        }
        os_ << "        " << branch_name << " = " << i << ",\n";
        used_branch_names.insert(branch_name);
    }
    os_ << "    };\n";

    os_ << "    size_t idx() const { return idx_; }\n";
    os_ << "    Branch branch() const { return static_cast<Branch>(idx_); }\n";

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        if (nn->type() == avro::AVRO_NULL) {
            os_ << "    bool is_null() const {\n"
                << "        return (idx_ == " << i << ");\n"
                << "    }\n"
                << "    void set_null() {\n"
                << "        idx_ = " << i << ";\n"
                << "        value_ = std::any();\n"
                << "    }\n";
        } else {
            const string &type = types[i];
            const string &name = names[i];
            os_ << "    "
                << "const " << type << "& get_" << name << "() const;\n"
                << "    " << type << "& get_" << name << "();\n"
                << "    void set_" << name << "(const " << type << "& v);\n"
                << "    void set_" << name << "(" << type << "&& v);\n";
            pendingGettersAndSetters.emplace_back(result, type, name, i);
        }
    }

    os_ << "    " << result << "();\n";
    pendingConstructors.emplace_back(result, types[0],
                                     n->leafAt(0)->type() != avro::AVRO_NULL);
    os_ << "};\n\n";

    return result;
}

/**
 * Returns the type for the given schema node and emits code to os.
 */
string CodeGen::generateType(const NodePtr &n) {
    NodePtr nn = (n->type() == avro::AVRO_SYMBOLIC) ? resolveSymbol(n) : n;

    map<NodePtr, string>::const_iterator it = done.find(nn);
    if (it != done.end()) {
        return it->second;
    }
    string result = doGenerateType(nn);
    done[nn] = result;
    return result;
}

string CodeGen::doGenerateType(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_STRING:
        case avro::AVRO_BYTES:
        case avro::AVRO_INT:
        case avro::AVRO_LONG:
        case avro::AVRO_FLOAT:
        case avro::AVRO_DOUBLE:
        case avro::AVRO_BOOL:
        case avro::AVRO_NULL:
        case avro::AVRO_FIXED:
            return cppTypeOf(n);
        case avro::AVRO_ARRAY: {
            const NodePtr &ln = n->leafAt(0);
            string dn;
            if (doing.find(n) == doing.end()) {
                doing.insert(n);
                dn = generateType(ln);
                doing.erase(n);
            } else {
                dn = generateDeclaration(ln);
            }
            return "std::vector<" + dn + " >";
        }
        case avro::AVRO_MAP: {
            const NodePtr &ln = n->leafAt(1);
            string dn;
            if (doing.find(n) == doing.end()) {
                doing.insert(n);
                dn = generateType(ln);
                doing.erase(n);
            } else {
                dn = generateDeclaration(ln);
            }
            return "std::map<std::string, " + dn + " >";
        }
        case avro::AVRO_RECORD:
            return generateRecordType(n);
        case avro::AVRO_ENUM:
            return generateEnumType(n);
        case avro::AVRO_UNION:
            return generateUnionType(n);
        default:
            break;
    }
    return "$Undefined$";
}

string CodeGen::generateDeclaration(const NodePtr &n) {
    NodePtr nn = (n->type() == avro::AVRO_SYMBOLIC) ? resolveSymbol(n) : n;
    switch (nn->type()) {
        case avro::AVRO_STRING:
        case avro::AVRO_BYTES:
        case avro::AVRO_INT:
        case avro::AVRO_LONG:
        case avro::AVRO_FLOAT:
        case avro::AVRO_DOUBLE:
        case avro::AVRO_BOOL:
        case avro::AVRO_NULL:
        case avro::AVRO_FIXED:
            return cppTypeOf(nn);
        case avro::AVRO_ARRAY:
            return "std::vector<" + generateDeclaration(nn->leafAt(0)) + " >";
        case avro::AVRO_MAP:
            return "std::map<std::string, " + generateDeclaration(nn->leafAt(1)) + " >";
        case avro::AVRO_RECORD:
            os_ << "struct " << cppTypeOf(nn) << ";\n";
            return cppTypeOf(nn);
        case avro::AVRO_ENUM:
            return generateEnumType(nn);
        case avro::AVRO_UNION:
            // FIXME: When can this happen?
            return generateUnionType(nn);
        default:
            break;
    }
    return "$Undefined$";
}

void CodeGen::generateEnumTraits(const NodePtr &n) {
    string dname = decorate(n->name());
    string fn = fullname(dname);

    // the nameAt(i) does not take c++ reserved words into account
    // so we need to call decorate on it
    string last = decorate(n->nameAt(n->names() - 1));

    os_ << "template<> struct codec_traits<" << fn << "> {\n"
        << "    static void encode(Encoder& e, " << fn << " v) {\n"
        << "        if (v > " << fn << "::" << last << ")\n"
        << "        {\n"
        << "            std::ostringstream error;\n"
        << R"(            error << "enum value " << static_cast<unsigned>(v) << " is out of bound for )" << fn
        << " and cannot be encoded\";\n"
        << "            throw avro::Exception(error.str());\n"
        << "        }\n"
        << "        e.encodeEnum(static_cast<size_t>(v));\n"
        << "    }\n"
        << "    static void decode(Decoder& d, " << fn << "& v) {\n"
        << "        size_t index = d.decodeEnum();\n"
        << "        if (index > static_cast<size_t>(" << fn << "::" << last << "))\n"
        << "        {\n"
        << "            std::ostringstream error;\n"
        << R"(            error << "enum value " << index << " is out of bound for )" << fn
        << " and cannot be decoded\";\n"
        << "            throw avro::Exception(error.str());\n"
        << "        }\n"
        << "        v = static_cast<" << fn << ">(index);\n"
        << "    }\n"
        << "};\n\n";
}

void CodeGen::generateRecordTraits(const NodePtr &n) {
    size_t c = n->leaves();
    for (size_t i = 0; i < c; ++i) {
        generateTraits(n->leafAt(i));
    }

    string fn = fullname(decorate(n->name()));
    os_ << "template<> struct codec_traits<" << fn << "> {\n";

    if (c == 0) {
        os_ << "    static void encode(Encoder&, const " << fn << "&) {}\n";
        // ResolvingDecoder::fieldOrder mutates the state of the decoder, so if that decoder is
        // passed in, we need to call the method even though it will return an empty vector.
        os_ << "    static void decode(Decoder& d, " << fn << "&) {\n";
        os_ << "        if (avro::ResolvingDecoder *rd = dynamic_cast<avro::ResolvingDecoder *>(&d)) {\n";
        os_ << "            rd->fieldOrder();\n";
        os_ << "        }\n";
        os_ << "    }\n";
        os_ << "};\n";
        return;
    }

    os_ << "    static void encode(Encoder& e, const " << fn << "& v) {\n";

    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "        avro::encode(e, v." << decoratedNameAt << ");\n";
    }

    os_ << "    }\n"
        << "    static void decode(Decoder& d, " << fn << "& v) {\n";
    os_ << "        if (avro::ResolvingDecoder *rd =\n";
    os_ << "            dynamic_cast<avro::ResolvingDecoder *>(&d)) {\n";
    os_ << "            const std::vector<size_t> fo = rd->fieldOrder();\n";
    os_ << "            for (std::vector<size_t>::const_iterator it = fo.begin();\n";
    os_ << "                it != fo.end(); ++it) {\n";
    os_ << "                switch (*it) {\n";
    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "                case " << i << ":\n";
        os_ << "                    avro::decode(d, v." << decoratedNameAt << ");\n";
        os_ << "                    break;\n";
    }
    os_ << "                default:\n";
    os_ << "                    break;\n";
    os_ << "                }\n";
    os_ << "            }\n";
    os_ << "        } else {\n";

    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "            avro::decode(d, v." << decoratedNameAt << ");\n";
    }
    os_ << "        }\n";

    os_ << "    }\n"
        << "};\n\n";
}

void CodeGen::generateUnionTraits(const NodePtr &n) {
    const string name = done[n];
    const string fn = fullname(name);
    if (unionTracker_.unionTraitsAlreadyGenerated(fn)) {
        return;
    }
    size_t c = n->leaves();

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        generateTraits(nn);
    }

    os_ << "template<> struct codec_traits<" << fn << "> {\n"
        << "    static void encode(Encoder& e, " << fn << " v) {\n"
        << "        e.encodeUnionIndex(v.idx());\n"
        << "        switch (v.idx()) {\n";

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        os_ << "        case " << i << ":\n";
        if (nn->type() == avro::AVRO_NULL) {
            os_ << "            e.encodeNull();\n";
        } else {
            os_ << "            avro::encode(e, v.get_" << cppNameOf(nn)
                << "());\n";
        }
        os_ << "            break;\n";
    }

    os_ << "        }\n"
        << "    }\n"
        << "    static void decode(Decoder& d, " << fn << "& v) {\n"
        << "        size_t n = d.decodeUnionIndex();\n"
        << "        if (n >= " << c << ") { throw avro::Exception(\""
                                       "Union index too big\"); }\n"
        << "        switch (n) {\n";

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        os_ << "        case " << i << ":\n";
        if (nn->type() == avro::AVRO_NULL) {
            os_ << "            d.decodeNull();\n"
                << "            v.set_null();\n";
        } else {
            os_ << "            {\n"
                << "                " << cppTypeOf(nn) << " vv;\n"
                << "                avro::decode(d, vv);\n"
                << "                v.set_" << cppNameOf(nn) << "(std::move(vv));\n"
                << "            }\n";
        }
        os_ << "            break;\n";
    }
    os_ << "        }\n"
        << "    }\n"
        << "};\n\n";

    unionTracker_.setTraitsGenerated(fn);
}

void CodeGen::generateTraits(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_STRING:
        case avro::AVRO_BYTES:
        case avro::AVRO_INT:
        case avro::AVRO_LONG:
        case avro::AVRO_FLOAT:
        case avro::AVRO_DOUBLE:
        case avro::AVRO_BOOL:
        case avro::AVRO_NULL:
            break;
        case avro::AVRO_RECORD:
            generateRecordTraits(n);
            break;
        case avro::AVRO_ENUM:
            generateEnumTraits(n);
            break;
        case avro::AVRO_ARRAY:
        case avro::AVRO_MAP:
            generateTraits(n->leafAt(n->type() == avro::AVRO_ARRAY ? 0 : 1));
            break;
        case avro::AVRO_UNION:
            generateUnionTraits(n);
            break;
        case avro::AVRO_FIXED:
        default:
            break;
    }
}

void CodeGen::generateDocComment(const NodePtr &n, const char *indent) {
    if (!n->getDoc().empty()) {
        std::vector<std::string> lines;
        {
            const std::string &doc = n->getDoc();
            size_t pos = 0;
            size_t found;
            while ((found = doc.find('\n', pos)) != std::string::npos) {
                lines.push_back(doc.substr(pos, found - pos));
                pos = found + 1;
            }
            if (pos < doc.size()) {
                lines.push_back(doc.substr(pos));
            }
        }
        for (auto &line : lines) {
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

            if (line.empty()) {
                os_ << indent << "//\n";
            } else {
                // If a comment line ends with a backslash or backslash and whitespace,
                // avoid generating code which will generate multi-line comment warnings
                // on GCC. We can't just append whitespace here as escaped newlines ignore
                // trailing whitespace.
                auto lastBackslash = std::find(line.rbegin(), line.rend(), '\\');
                auto lastNonWs = std::find_if(line.rbegin(), line.rend(), [](char c) { return !std::isspace(static_cast<int>(c)); });
                // Note: lastBackslash <= lastNonWs because the iterators are reversed, "less" is later in the string.
                if (lastBackslash != line.rend() && lastBackslash <= lastNonWs) {
                    line.append("(backslash)");
                }
                os_ << indent << "// " << line << "\n";
            }
        }
    }
}

void CodeGen::emitCopyright() {
    os_ << "/**\n"
           " * Licensed to the Apache Software Foundation (ASF) under one\n"
           " * or more contributor license agreements.  See the NOTICE file\n"
           " * distributed with this work for additional information\n"
           " * regarding copyright ownership.  The ASF licenses this file\n"
           " * to you under the Apache License, Version 2.0 (the\n"
           " * \"License\"); you may not use this file except in compliance\n"
           " * with the License.  You may obtain a copy of the License at\n"
           " *\n"
           " *     https://www.apache.org/licenses/LICENSE-2.0\n"
           " *\n"
           " * Unless required by applicable law or agreed to in writing, "
           "software\n"
           " * distributed under the License is distributed on an "
           "\"AS IS\" BASIS,\n"
           " * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express "
           "or implied.\n"
           " * See the License for the specific language governing "
           "permissions and\n"
           " * limitations under the License.\n"
           " */\n\n";
}

void CodeGen::emitGeneratedWarning() {
    os_ << "/* This code was generated by avrogencpp " << AVRO_VERSION << ". Do not edit.*/\n\n";
}

string CodeGen::guard() {
    string h = headerFile_;
    makeCanonical(h, true);
    return h + "_" + std::to_string(random_()) + "_H";
}

void CodeGen::generate(const ValidSchema &schema) {
    emitCopyright();
    emitGeneratedWarning();

    string h = guardString_.empty() ? guard() : guardString_;

    os_ << "#ifndef " << h << "\n";
    os_ << "#define " << h << "\n\n\n";

    os_ << "#include <sstream>\n"
        << "#include <any>\n"
        << "#include <utility>\n"
        << "#include \"" << includePrefix_ << "Specific.hh\"\n"
        << "#include \"" << includePrefix_ << "Encoder.hh\"\n"
        << "#include \"" << includePrefix_ << "Decoder.hh\"\n"
        << "\n";

    if (!ns_.empty()) {
        os_ << "namespace " << ns_ << " {\n";
        inNamespace_ = true;
    }

    const NodePtr &root = schema.root();
    generateType(root);

    for (vector<PendingSetterGetter>::const_iterator it =
             pendingGettersAndSetters.begin();
         it != pendingGettersAndSetters.end(); ++it) {
        generateGetterAndSetter(os_, it->structName, it->type, it->name,
                                it->idx);
    }

    for (vector<PendingConstructor>::const_iterator it =
             pendingConstructors.begin();
         it != pendingConstructors.end(); ++it) {
        generateConstructor(os_, it->structName,
                            it->initMember, it->memberName);
    }

    if (!ns_.empty()) {
        inNamespace_ = false;
        os_ << "}\n";
    }

    os_ << "namespace avro {\n";

    generateTraits(root);

    os_ << "}\n";

    os_ << "#endif\n";
    os_.flush();
}

static string readGuard(const string &filename) {
    std::ifstream ifs(filename.c_str());
    string buf;
    string candidate;
    while (std::getline(ifs, buf)) {
        if (!buf.empty()) {
            size_t start = 0, end = buf.length();
            while (start < end && std::isspace(buf[start], std::locale::classic())) start++;
            while (start < end && std::isspace(buf[end - 1], std::locale::classic())) end--;
            if (start > 0 || end < buf.length()) {
                buf = buf.substr(start, end - start);
            }
        }
        if (candidate.empty()) {
            if (buf.compare(0, 8, "#ifndef ") == 0) {
                candidate = buf.substr(8);
            }
        } else if (buf.compare(0, 8, "#define ") == 0) {
            if (candidate == buf.substr(8)) {
                break;
            }
        } else {
            candidate.erase();
        }
    }
    return candidate;
}

struct ProgramOptions {
    bool helpRequested = false;
    bool versionRequested = false;
    bool noUnionTypedef = false;
    std::string includePrefix = "avro";
    std::string nameSpace;
    std::string inputFile;
    std::string outputFile;
};

static void printUsage() {
    std::cout << "Allowed options:\n"
              << "  -h [ --help ]                       produce help message\n"
              << "  -V [ --version ]                    produce version information\n"
              << "  -p [ --include-prefix ] arg (=avro) prefix for include headers, - for none, default: avro\n"
              << "  -U [ --no-union-typedef ]           do not generate typedefs for unions in records\n"
              << "  -n [ --namespace ] arg              set namespace for generated code\n"
              << "  -i [ --input ] arg                  input file\n"
              << "  -o [ --output ] arg                 output file to generate\n";
}

static bool parseArgs(int argc, char **argv, ProgramOptions &opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.helpRequested = true;
            return true;
        }

        if (arg == "-V" || arg == "--version") {
            opts.versionRequested = true;
            return true;
        }

        if (arg == "-U" || arg == "--no-union-typedef") {
            opts.noUnionTypedef = true;
            continue;
        }

        if (arg == "-p" || arg == "--include-prefix") {
            if (i + 1 < argc) {
                opts.includePrefix = argv[++i];
                continue;
            }
        } else if (arg == "-n" || arg == "--namespace") {
            if (i + 1 < argc) {
                opts.nameSpace = argv[++i];
                continue;
            }
        } else if (arg == "-i" || arg == "--input") {
            if (i + 1 < argc) {
                opts.inputFile = argv[++i];
                continue;
            }
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                opts.outputFile = argv[++i];
                continue;
            }
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            return false;
        }

        std::cerr << "Missing value for option: " << arg << std::endl;
        return false;
    }

    return true;
}

int main(int argc, char **argv) {
    ProgramOptions opts;
    if (!parseArgs(argc, argv, opts)) {
        printUsage();
        return 1;
    }

    if (opts.helpRequested) {
        printUsage();
        return 0;
    }

    if (opts.versionRequested) {
        std::cout << AVRO_VERSION << std::endl;
        return 0;
    }

    if (opts.inputFile.empty() || opts.outputFile.empty()) {
        std::cerr << "Input and output files are required.\n\n";
        printUsage();
        return 1;
    }

    std::string ns = opts.nameSpace;
    std::string outf = opts.outputFile;
    std::string inf = opts.inputFile;
    std::string incPrefix = opts.includePrefix;
    bool noUnion = opts.noUnionTypedef;

    if (incPrefix == "-") {
        incPrefix.clear();
    } else if (*incPrefix.rbegin() != '/') {
        incPrefix += "/";
    }

    try {
        ValidSchema schema;

        if (!inf.empty()) {
            ifstream in(inf.c_str());
            compileJsonSchema(in, schema);
        } else {
            compileJsonSchema(std::cin, schema);
        }

        if (!outf.empty()) {
            string g = readGuard(outf);
            ofstream out(outf.c_str());
            CodeGen(out, ns, inf, outf, g, incPrefix, noUnion).generate(schema);
        } else {
            CodeGen(std::cout, ns, inf, outf, "", incPrefix, noUnion).generate(schema);
        }
        return 0;
    } catch (std::exception &e) {
        std::cerr << "Failed to parse or compile schema: "
                  << e.what() << std::endl;
        return 1;
    }
}

UnionCodeTracker::UnionCodeTracker(const std::string &schemaFile) : schemaFile_(schemaFile) {
}

std::optional<std::string> UnionCodeTracker::getExistingUnionName(const std::vector<std::string> &unionBranches) const {
    if (const auto it = unionBranchNameMapping_.find(unionBranches); it != unionBranchNameMapping_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string UnionCodeTracker::generateNewUnionName(const std::vector<std::string> &unionBranches) {
    string s = schemaFile_;
    string::size_type n = s.find_last_of("/\\");
    if (n != string::npos) {
        s = s.substr(n);
    }
    makeCanonical(s, false);

    std::string result = s + "_Union__" + std::to_string(unionNumber_++) + "__";
    unionBranchNameMapping_.emplace(unionBranches, result);
    return result;
}

bool UnionCodeTracker::unionTraitsAlreadyGenerated(const std::string &unionClassName) const {
    return generatedUnionTraits_.find(unionClassName) != generatedUnionTraits_.end();
}

void UnionCodeTracker::setTraitsGenerated(const std::string &unionClassName) {
    generatedUnionTraits_.insert(unionClassName);
}
