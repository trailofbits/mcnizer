//
// Created by sabastiaan on 22-06-21.
//
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"


#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include <system_error>
#include <iostream>
#include <sstream>
#include <fstream>
#include "llvm/IR/LegacyPassManager.h"



#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "llvm/Transforms/Utils.h"
#include <filesystem>


using namespace llvm;
static ExitOnError ExitOnErr;

static cl::opt<std::string>
        InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));


// note that the functions using this name should handle path construction so don't suffix this with /
const std::string OUTPUT_DIR = "output";


int iterator_names_used = 0;
std::vector<std::string> iterator_names;
std::string getUniqueLoopIteratorName(){
    std::string name = "i" + std::to_string(iterator_names_used);
    iterator_names.push_back(name);
    iterator_names_used++;
    return name;
}



std::string getCTypeNameForLLVMType(Type* type){
    if(type->isPointerTy())
        return getCTypeNameForLLVMType(type->getPointerElementType()) + "*";

//    bools get converted to i8 in llvm
//    if(type->isIntegerTy(1))
//        return "bool";

    if(type->isIntegerTy(8))
        return "char";

    if(type->isIntegerTy(16))
        return "int16_t";

    if(type->isIntegerTy(32))
        return "int32_t";

    if(type->isIntegerTy(64))
        return "int64_t";

//    if(type->isIntegerTy(64))
//        return "long long"; // this makes very strong assumptions on underling structures, should fix this

    if(type->isVoidTy())
        return "void";

    if(type->isFloatTy())
        return "float";

    if(type->isFloatTy())
        return "double";

    if(type->isStructTy()){
        std::string s = type->getStructName();
        if(s.find("struct.") != std::string::npos)
            s = s.replace(0, 7, ""); //removes struct. prefix

        else if (s.find("union.") != std::string::npos)
            s = s.replace(0, 6, ""); //removes union. prefix
        return s;
    }

    if(type->isArrayTy())
        throw std::invalid_argument("Arrays can't have their names expressed like this");

    return "Not supported";
}


// llvm arguments are context bound so we can't use them as freely
// therefore we keep all info in a custom structure
// should it include a list for structs S.T each struct has members+name, and can I use that setup for scalar values as well?
class handarg {
public:
    std::string name;
    int position;
    Type* type;
    bool passByVal;

    handarg(std::string name, int position, Type* type, bool passByVal = false){
        this->name = name;
        this->position = position;
        this->type = type;
        this->passByVal = passByVal;
    };

    //mirror some LLVM type functions
    Type* getType(){ return this->type;};
    Type* getTypeOrPointedType(){
        if(this->type->isPointerTy())
            return this->type->getPointerElementType();
        return this->type;
    };
    bool isStructOrStructPtr(){ return this->type->isStructTy() || (this->type->isPointerTy() && this->type->getPointerElementType()->isStructTy());}
    std::string getName(){ return this->name;};
};



// if we encounter a struct or union type we must define it first in our program
// we do this through a DFS on the elements of every argument we find
// as long as the result of every DFS search on another root/non-recursive call is appended AFTER the previous result
// we won't get into a nasty dependency problem. This follows trivially from that at each point
// we either generate direct dependencies or they are already defined before hand.
// To not re-define previously defined structs we save the name in an vector



// Types in llvm are unique, therefore we can create an bijective DefinedStruct -> StructType
// I hope this uniqueness is also for structs
// and save all the member names in DefinedStruct
class DefinedStruct {
public:
    bool isUnion = false;
    StructType* structType;
    std::string definedCStructName;
    std::vector<std::string> member_names; // implicitly ordered, can I make an tuple out of this combined with the member?

    std::vector<std::pair<std::string, Type*>> getNamedMembers() {
        std::vector<std::pair<std::string, Type*>> output;
        if(member_names.size() != structType->getNumElements())
            std::throw_with_nested("Not all members are named");

        for(int i = 0; i < structType->getNumElements(); i++){
            output.push_back(std::pair<std::string, Type*>(member_names[i], structType->getStructElementType(i)));

        }
        return output;
    };
};

std::string getSetupFileText(std::string functionName, Type *funcRetType, std::vector<handarg> &args_of_func,
                             std::vector<handarg> &globals);

std::vector<handarg> extractArgumentsFromFunction(Function &f);

void GenerateJsonInputTemplateFile(const std::string &funcName, std::vector<handarg> &args_of_func,
                                   std::vector<handarg> &globals);

std::vector<handarg> extractGlobalValuesFromModule(std::unique_ptr<Module> &mod);

void GenerateCppFunctionHarness(std::string &funcName, Type *funcRetType, std::vector<handarg> &args_of_func,
                                std::vector<handarg> &globals_of_func);

enum StringFormat{
    GENERATE_FORMAT_CPP_ADDRESSING,
    GENERATE_FORMAT_CPP_VARIABLE,
    GENERATE_FORMAT_JSON_ARRAY_ADDRESSING
};

const std::string CPP_ADDRESSING_DELIMITER = ".";
const std::string LVALUE_DELIMITER = "_";
const std::string POINTER_DENOTATION = "__p";
const std::string ARRAY_INDEX_TOKEN = "i";
// so big idea is to generate all source pieces from list<args>
// with args = struct{ LLVMType, position, name}, and at each c++ location we need one to call ConvertLLVMTypeToC++,
/*
 * Naming in an autogenerated context in which we want both descriptive names and don't easily know all names beforehand suck
 * Eventhough naming from globals and function arguments are trivial, we have to deal with temps for custom classes, with any number of named members which all can also be a custom or autogenerated name
 * To prevent any name clashes we settled on the following
 *
 * We keep a vector of names, one entry for how nested the variable is in an object hierarchy (i.e for struct Y { int a; }, struct X { Y i}; we have <name of x var, i, a>
 *
 */
std::string joinStrings(std::vector<std::string> strings, StringFormat format){
    std::stringstream output;
    std::string delimiter = "";
    if(format == GENERATE_FORMAT_CPP_ADDRESSING)
        delimiter = CPP_ADDRESSING_DELIMITER;
    if(format == GENERATE_FORMAT_CPP_VARIABLE)
        delimiter = LVALUE_DELIMITER;


    for(std::string s : strings){
        if(format != GENERATE_FORMAT_CPP_VARIABLE && s == POINTER_DENOTATION)
            continue; // don't print the pointer markings in lvalue as these are abstracted away
        if(format == GENERATE_FORMAT_JSON_ARRAY_ADDRESSING){
            if(std::find(iterator_names.begin(), iterator_names.end(), s) != iterator_names.end())
                output << "[" << s << "]";
            else
                output << "[\"" << s << "\"]";
        }
        else{
            output << s << delimiter;
        }
    }

    auto retstring = output.str();
    if(retstring.length() > 0 && format != GENERATE_FORMAT_JSON_ARRAY_ADDRESSING)
        retstring.pop_back(); //remove trailing delimiter
    return retstring;
}

// Unions get translated to structs in LLVM so this should also cover that
std::vector<DefinedStruct> definedStructs;


bool isStructAlreadyDefined(StructType* type){
    for(auto s : definedStructs){
        if(s.structType == type)
            return true;
    }
    return false;
}



DefinedStruct getStructByLLVMType(StructType* structType){
    for(auto s : definedStructs){
        if(s.structType == structType)
            return s; // will this deep copy the vector as well?
    }
    std::throw_with_nested("Struct is not defined");
}
std::stringstream definitionStrings;

// Adds itself in the correct order to definitionStream
void defineIfNeeded(Type* arg, bool isRetType = false){
    // if we have a T**** we might still need to define T, so recurse instead of check
    if(arg->isPointerTy())
        defineIfNeeded(arg->getPointerElementType(), isRetType);

    std::cout << "called for: " << getCTypeNameForLLVMType(arg);
    if(arg->isStructTy()){ // we don't care about arrays thus no (is aggregrate type)
        std::cout << " Found a struct type";

        if(isStructAlreadyDefined((StructType*) arg))
            return;
        std::cout << "Not defined";
        if(arg->getStructNumElements() > 0){
            std::cout << "Has members";
            std::stringstream  elementsString;
            DefinedStruct newDefinedStruct;
            if(arg->getStructName().find("union.") != std::string::npos)
                newDefinedStruct.isUnion = true;

            // all members need to be added, if any struct is referenced we need tto define it again
            for(int i =0 ; i < arg->getStructNumElements(); i++){
                auto child = arg->getStructElementType(i);

                /*
                 * A struct can contain 3 constructs which we might need to define
                 * 1. pointer should always be tried to be defined
                 * 2. a struct obviously
                 * 3. array types
                 */
                if (child->isStructTy() || child->isPointerTy())
                    defineIfNeeded(child);
                if (child->isArrayTy()) {
                    auto arrType = child->getArrayElementType();
                    if (arrType->isStructTy() || arrType->isPointerTy())
                        defineIfNeeded(arrType);
                }

                std::string childname = "e_" + std::to_string(i);
                if(child->isArrayTy()){ // of course we can't have nice things in this world :(
                    auto arrayType = child->getArrayElementType();
                    auto arraySize = child->getArrayNumElements();
                    elementsString << "\t" << getCTypeNameForLLVMType(arrayType) << " " << childname  << "[" << arraySize << "];" << std::endl;
                }
                else{
                    elementsString << "\t" << getCTypeNameForLLVMType(child) << " " << childname  << ";" << std::endl;
                }

                newDefinedStruct.member_names.push_back(childname);
            }

            // TODO: use getCPPName instead of getStructName?
            auto structName = getCTypeNameForLLVMType(arg);
            if(isRetType)
                structName = "returnType";
            newDefinedStruct.structType = (StructType*) arg;
            if(newDefinedStruct.isUnion)
                definitionStrings << "typedef union ";
            else
                definitionStrings << "typedef struct ";
            definitionStrings << structName << " { " << std::endl << elementsString.str() << "} " << structName << ";" <<  std::endl <<  std::endl;
            newDefinedStruct.definedCStructName = structName;
            definedStructs.push_back(newDefinedStruct);
        }
    }
}

std::string getJSONStructDeclartions(){
    std::stringstream s;

    for(auto& ds : definedStructs){
        s << "NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(";
        s << ds.definedCStructName << ", ";
        for(auto& mem : ds.getNamedMembers()){
            s << mem.first << ", ";
        }
        s.seekp(-2,s.cur); // removes trialing , by replacing it with )
        s << ")" << std::endl;
    }
    return s.str();
}

void defineStructs(std::vector<handarg> args){
    for(auto& a : args){
        defineIfNeeded(a.getType());
    }
}

std::string getStructDefinitions(){
    return definitionStrings.str();
}


std::string getDefineGlobalsText(std::vector<handarg> globals){
    std::stringstream s;
    for(auto& global : globals){
        s << "extern " << getCTypeNameForLLVMType(global.getType()) << " " << global.getName() << ";" << std::endl;
    }

    return s.str();
}


// thus we only need a function which converts has a one-to-one with LLVM to c++ types,

std::string getParserSetupTextForScalarType(std::string name, Type* type, bool isGlobal = false){
    std::stringstream s;
    std::string requiredString = ".required()";
    if(isGlobal){
        name = "global." + name;
        requiredString = ""; //dirty hack
    }
    std::string action = ""; //parsing method

    if(type->isIntegerTy(8))
        action = "parseCharInput";

    if(type->isIntegerTy(16))
        action = "parseShortInput";

    if(type->isIntegerTy(32))
        action = "[](const std::string& value) {return stoi(value);}";

    if(type->isIntegerTy(64))
        action = "[](const std::string& value) {return stol(value);}";

    if(type->isFloatTy())
        action = "[](const std::string& value) {return stof(value);}";

    if(type->isDoubleTy())
        action = "[](const std::string& value) {return stod(value);}";


    if(type->isIntegerTy(8) || type->isIntegerTy(16) || type->isIntegerTy(32) || type->isIntegerTy(64) || type->isFloatTy() || type->isDoubleTy()){
        s << "\t\t" << "parser.add_argument(\"--" << name << "\")" << requiredString <<
                                                           ".help(\"" << getCTypeNameForLLVMType(type) << "\")" <<
                                                           ".action(" << action << ");" << std::endl;
    }

    return s.str();
}




std::string getParserSetupTextFromLLVMTypes(std::vector<std::string> name_prefix, Type* arg, bool isGlobal = false){
    std::stringstream s;

    if(arg->isStructTy()){
        auto str = getStructByLLVMType((StructType*) arg);
        for(auto& mem : str.getNamedMembers()){
            std::vector<std::string> memberName(name_prefix);
            memberName.push_back(mem.first);
            s << getParserSetupTextFromLLVMTypes(memberName, mem.second, isGlobal);
        }
    }

    if(arg->isPointerTy() && arg->getPointerElementType()->isStructTy()){
//        auto structType = arg;
        auto str = getStructByLLVMType((StructType*) arg->getPointerElementType());
        for(auto& mem : str.getNamedMembers()){
            std::vector<std::string> memberName(name_prefix);
            memberName.push_back(mem.first);
            s << getParserSetupTextFromLLVMTypes(memberName, mem.second, isGlobal); // but we need to maintain the counter
        }
//            std::string argname = name_prefix + "p" + std::to_string(counter);
        // we dont encode pointers in name so just call recursively

    }
    else{
        if(arg->isPointerTy())
            s << getParserSetupTextFromLLVMTypes(name_prefix, arg->getPointerElementType(), isGlobal); // but we need to maintain the counter
        else
            s << getParserSetupTextForScalarType(joinStrings(name_prefix, GENERATE_FORMAT_CPP_ADDRESSING), arg, isGlobal);
    }


    return s.str();
}

// should include position in loop instead of relying on implicit order
// wrapper around getParserSetupTextFromType
std::string getParserSetupTextFromHandargs(std::vector<handarg> args, bool isForGlobals = false){
    std::stringstream s;
    for(auto& a : args){
        std::vector<std::string> name;
        name.push_back(a.getName());
        s << getParserSetupTextFromLLVMTypes(name,a.type, isForGlobals);
    }
    return s.str();
}


std::string getStringFromJson(std::string output_var, std::string json_name, std::string tmp_name_1, std::string tmp_name_2, bool addNullTermination = true){
    std::stringstream s;
    s << "std::string " << tmp_name_1 << " = " << json_name << ".get<std::string>();" << std::endl;
    s << "std::vector<char> " << tmp_name_2 << "(" << tmp_name_1 << ".begin(), " << tmp_name_1 << ".end());";
    if(addNullTermination)
        s << tmp_name_2 << ".push_back('\\x00');" << std::endl;
    s << "char* " << output_var << " = &" << tmp_name_2 << "[0];" << std::endl;
    return s.str();
}


std::string getStringLengthFromJson(std::string output_var, std::string json_name, std::string tmp_name_1, std::string tmp_name_2, bool addNullTermination = true){
    std::stringstream s;
    s << "std::string " << tmp_name_1 << " = " << json_name << ".get<std::string>();" << std::endl;
    s << "std::vector<char> " << tmp_name_2 << "(" << tmp_name_1 << ".begin(), " << tmp_name_1 << ".end());";
    if(addNullTermination)
        s << tmp_name_2 << ".push_back('\x00');" << std::endl;
    s << "int " << output_var << " = " << tmp_name_2 << ".size();" << std::endl;
    return s.str();
}

std::vector<std::string> used_tmps;

// a string is unique mostly for a string path like this
std::string getUniqueCppTmpString(std::vector<std::string> prefixes){
    auto s = joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE);
    used_tmps.push_back(s);
    s = s + std::to_string(used_tmps.size()); // really shitty implementation but  works for now
    return s;
}


// Names are constructed the following
// Consider an actual argumetn to the tested function, and call their name root
// For any scalar type we just print the name
// If its a struct all member names need to following convention
// <type> assignment_name = parser.get<type>(parsername)
// where assignment name is delimited with _ and parsername with .
// This way we can keep going deeper in the hierarcy but just adding the latest struct name
// and forfill both conventions

/*
 * assignment_name is always prefixes.joined('_')
 * as assignment name has to deal with multiple pointer levels, we add a pointer depth
 * notice that we cannot encode how many extra variables we have made to deal with pointers as we need to keep prefixes only reflect structure depth
 * otherwise the parser.get("--name") we would diverge with the setup
 */
std::string getParserRetrievalForNamedType(std::vector<std::string> prefixes, Type* type, bool isForGlobals = false){
    std::stringstream output;
    // arrays only exists in types unless they are global
    if(isForGlobals && type->isArrayTy()){
        int arrSize = type->getArrayNumElements();
        Type* arrType = type->getArrayElementType();
        std::string iterator_name = getUniqueLoopIteratorName(); // we require a name from this function to get proper generation
        output << "for(int " << iterator_name << " = 0; " << iterator_name << " < "<< arrSize << ";" << iterator_name << "++) {" << std::endl;
        std::vector<std::string> fullname(prefixes);
        fullname.push_back(iterator_name);
        output << getParserRetrievalForNamedType(fullname, arrType, false); // we declare a new local variable so don't pass in the global flag
        output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << "[" << iterator_name << "] = " << joinStrings(fullname, GENERATE_FORMAT_CPP_VARIABLE) << ";" << std::endl;
        output << "}" << std::endl;
    }


    // Pointers recurse until they point to a non pointer
    // for a non-pointer it declares the base type and sets it as an array/scalar
    else if(type->isPointerTy()) {
        if (type->getPointerElementType()->isPointerTy()) {
            std::vector<std::string> referenced_name(prefixes);
            referenced_name.push_back(POINTER_DENOTATION);
            output << getParserRetrievalForNamedType(referenced_name, type->getPointerElementType(), isForGlobals);

            output << "\t\t";
            if (!isForGlobals)
                output << getCTypeNameForLLVMType(type) << " ";
            output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << " = &"
                   << joinStrings(referenced_name, GENERATE_FORMAT_CPP_VARIABLE) << ";" << std::endl;
        }
        else { // we know ptr points to a non pointer
            Type* pointeeType = type->getPointerElementType();
            std::string elTypeName = getCTypeNameForLLVMType(pointeeType);
            std::string arrSize =  "j" + joinStrings(prefixes, GENERATE_FORMAT_JSON_ARRAY_ADDRESSING) + ".size()";
            std::string jsonValue = "j" + joinStrings(prefixes, GENERATE_FORMAT_JSON_ARRAY_ADDRESSING);
            //declare var, as we recurse it must always be an extra definition
            output << elTypeName << "* " << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << ";" << std::endl;


            // normal behavior
            if (!pointeeType->isIntegerTy(8)){
                // if its an array
                output << "if(" << jsonValue << ".is_array()) { " << std::endl;
                std::vector<std::string> malloced_value(prefixes);
                malloced_value.push_back("malloced");


                output << elTypeName << "* " << joinStrings(malloced_value, GENERATE_FORMAT_CPP_VARIABLE);

                // TODO: should add a corresponding free
                output << " = (" << elTypeName << "*) malloc(sizeof(" << elTypeName << ") * " << arrSize << ");" << std::endl;

                std::string iterator_name = getUniqueLoopIteratorName(); // we require a name from this function to get proper generation
                output << "for(int " << iterator_name << " = 0; " << iterator_name << " < "<< arrSize << ";" << iterator_name << "++) {" << std::endl;
                output << joinStrings(malloced_value, GENERATE_FORMAT_CPP_VARIABLE) << "[" << iterator_name << "] = j" << joinStrings(prefixes, GENERATE_FORMAT_JSON_ARRAY_ADDRESSING) << "[" << iterator_name << "].get<" << elTypeName << ">();" << std::endl;
                output << "}" << std::endl; //endfor

                output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << " = " << joinStrings(malloced_value, GENERATE_FORMAT_CPP_VARIABLE) << ";" <<std::endl;
                output << "}" << std::endl; //end if_array

                // if its a number
                output << "if(" << jsonValue << ".is_number()){" << std::endl;
                //cast rvalue in case its a char
                auto stack_alloced_name = joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) + "_stack_alloced";
                output << elTypeName << " " <<  stack_alloced_name << "= (" << elTypeName << ")" << jsonValue << ".get<" << elTypeName << ">();" << std::endl;
                output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << " = &" << stack_alloced_name << ";" << std::endl;
                output << "}" << std::endl; //end if number

            }
            else{ // char behavior
                /*
                 * we handle chars in 3 different ways, l and rvalues here are in hson
                 * char_value = "string" => null terminated
                 * char_value = 70 => ascii without null
                 * char_value = ["f", "o", NULL , "o"] => in memory this should reflect f o NULL o, WITHOUT EXPLICIT NULL TERMINATION!
                 * char_value = ["fooo", 45, NULL, "long string"], same rules apply here as above, this should not have a null all the way at the end
                 */


                output << "if(" << jsonValue << ".is_string()) {" << std::endl;

                std::string string_tmp_val = getUniqueCppTmpString(prefixes);
//                output << "std::string " << joinStrings(string_tmp_val, GENERATE_FORMAT_CPP_VARIABLE) << " = " << jsonValue << ".get<std::string>();" << std::endl;

                //we can't use c_str here because if the string itself contains null, then it it is allowed to misbehave
                output << getStringFromJson(string_tmp_val, jsonValue, getUniqueCppTmpString(prefixes),
                                            getUniqueCppTmpString(prefixes));

                output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << " = " << string_tmp_val << ";" << std::endl;
                output << "}" << std::endl; // end is_string


                output << "if(" << jsonValue << ".is_number()) {" << std::endl;
                auto tmp_char_val = getUniqueCppTmpString(prefixes);
                output << "char " << tmp_char_val << " = " << jsonValue << ".get<char>();" << std::endl; // TODO: no idea if it accepts .get<char>
                output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << " = &" << tmp_char_val << ";" << std::endl;
                output << "}" << std::endl; //end is_number


                output << "if(" << jsonValue << ".is_array()) {" << std::endl;
                auto nestedIterator =  getUniqueLoopIteratorName();
                auto nestedIteratorIndex ="[" + nestedIterator + "]";
                auto indexedJsonValue = jsonValue + nestedIteratorIndex;
                output << "int malloc_size_counter = 0;" << std::endl;
                output << "for(int " << nestedIterator << " = 0; " << nestedIterator << " < " << jsonValue << ".size(); " << nestedIterator << "++){ " << std::endl;
                output << "if(" << indexedJsonValue << ".is_number() || " << jsonValue << nestedIteratorIndex << ".is_null()) malloc_size_counter++;"; // assume its a char
                output << "if(" << indexedJsonValue << ".is_string()){"; // assume its a byte
                auto str_len_counter = getUniqueCppTmpString(prefixes);
                output << getStringLengthFromJson(str_len_counter, indexedJsonValue, // don't trust .size for nulled strings
                                                  getUniqueCppTmpString(prefixes), getUniqueCppTmpString(prefixes), false);
                output << "malloc_size_counter = malloc_size_counter + " << str_len_counter << ";" << std::endl;
                output << "}" << std::endl; // end is_string


                output << "}" << std::endl; // end for
                // first loop to get the total size
                // then copy

                auto output_buffer_name = getUniqueCppTmpString(prefixes);
                auto writeHeadName = getUniqueCppTmpString(prefixes); // writes to this offset in the buffer
                auto indexed_output_buffer_name = output_buffer_name + "[" + writeHeadName + "]";
                output << "char* " << output_buffer_name << " = (char*)malloc(sizeof(char) * malloc_size_counter);" << std::endl;
                output << "int " << writeHeadName << " = 0;" << std::endl;
                //here copy, loop again over all and write

                output << "for(int " << nestedIterator << " = 0; " << nestedIterator << " < " << jsonValue << ".size(); " << nestedIterator << "++){ " << std::endl;
                output << "if(" << jsonValue << nestedIteratorIndex << ".is_string()){ ";
                    auto str_val = getUniqueCppTmpString(prefixes);
                    output << "std::string " << str_val << " = " << indexedJsonValue << ".get<std::string>();";
                    // copy the string into the correct position
                    // .copy guarantees to not add an extra null char at the end which is what we desire
                    auto charsWritten  = getUniqueCppTmpString(prefixes);
                    // TODO: Check if this indeed copys part way into the buffer
                    output << "auto " << charsWritten << " = " << str_val << ".copy(&" << indexed_output_buffer_name << ", " << str_val << ".size(), " << "0);" << std::endl;
                    output << writeHeadName << " = " << writeHeadName << " + " << charsWritten << ";" << std::endl;
                output << "}" << std::endl; // end is string

                output << "if(" << jsonValue << nestedIteratorIndex << ".is_null()){ ";
                output << indexed_output_buffer_name << " = '\\x00';" << std::endl;
                output << writeHeadName << "++;" << std::endl;
                output << "}" << std::endl; // end is string

                output << "if(" << jsonValue << nestedIteratorIndex << ".is_number()){ ";

                //TODO: Maybe add some error handling here
                output << indexed_output_buffer_name << " = (char)" << indexedJsonValue << ".get<int>();" << std::endl;
                output << writeHeadName << "++;" << std::endl;
                output << "}" << std::endl; // end is string


                // if a string copy for length?

                output << "}" << std::endl; // end for

                output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << " = " << output_buffer_name << ";" << std::endl; // TODO: no idea if it accepts .get<char>
                output << "}" << std::endl; // end is_array
            }



            // char array stuff
        }
    }


    // if type is scalar => output directly
    if(type->isIntegerTy(1) || type->isIntegerTy(8) || type->isIntegerTy(16) || type->isIntegerTy(32) || type->isIntegerTy(64)){
        output << "\t\t";
        std::string parserArg = joinStrings(prefixes, GENERATE_FORMAT_CPP_ADDRESSING);

        if(!isForGlobals) // only declare types if its not global, otherwise we introduce them as local variable
            output << getCTypeNameForLLVMType(type) << " ";
        else {

            parserArg = "global." + parserArg;
        }
        //declare lvalue
        output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE);
        output << " = j" << joinStrings(prefixes, GENERATE_FORMAT_JSON_ARRAY_ADDRESSING) << ".get<" << getCTypeNameForLLVMType(type) << ">();";
        output << std::endl;
    }

    // if type is struct, recurse with member names added as prefix
    if(type->isStructTy()){
        // first do non array members
        // declare and assign values to struct
        // then fill in struct arrays (as these can only be filled in post declaration)


        DefinedStruct ds = getStructByLLVMType((StructType*)type);
        for(auto member : ds.getNamedMembers()){
            if(member.second->isArrayTy() == false){
                std::vector<std::string> fullMemberName(prefixes);
                fullMemberName.push_back(member.first);
                output << getParserRetrievalForNamedType(fullMemberName, member.second, false);
            }
        }
        output << "\t";
        if(!isForGlobals)
            output << ds.definedCStructName << " ";
        output  << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE);
        if(isForGlobals)
            output << " = { " << std::endl; // with globals we have different syntax :?
        else
            output << "{ " << std::endl;
        for(auto member : ds.getNamedMembers()){
            if(member.second->isArrayTy() == false){
                std::vector<std::string> fullMemberName(prefixes);
                fullMemberName.push_back(member.first);
                //TODO formalize the fullmember name (lvalue name for member)
                output << "\t\t" << "." << member.first << " = " << joinStrings(fullMemberName, GENERATE_FORMAT_CPP_VARIABLE) << "," << std::endl;
            }
        }
        output << "\t};" << std::endl;

        for(auto member : ds.getNamedMembers()){
            // arrays can be also of custom types with arbitrary but finite amounts of nesting, so we need to recurse
            if(member.second->isArrayTy()){
                int arrSize = member.second->getArrayNumElements();
                Type* arrType = member.second->getArrayElementType();
                std::string iterator_name = getUniqueLoopIteratorName(); // we require a name from this function to get proper generation
                output << "for(int " << iterator_name << " = 0; " << iterator_name << " < "<< arrSize << ";" << iterator_name << "++) {" << std::endl;
                    std::vector<std::string> fullMemberName(prefixes);
                    fullMemberName.push_back(member.first);
                    fullMemberName.push_back(iterator_name);
                    output << getParserRetrievalForNamedType(fullMemberName, arrType, false);
                    output << joinStrings(prefixes, GENERATE_FORMAT_CPP_VARIABLE) << "." << member.first << "[" <<  iterator_name << "] = " << joinStrings(fullMemberName, GENERATE_FORMAT_CPP_VARIABLE) << ";" << std::endl;
                output << "}" << std::endl;
            }
        }
    }
    return output.str();
}

std::string getParserRetrievalText(std::vector<handarg> args, bool isForGlobals = false){
    std::stringstream s;

    for(auto& a : args){
        std::vector<std::string> dummy;
        dummy.push_back(a.getName());

        // llvm boxes structs always with a pointer?
        if(a.type->isPointerTy() && a.type->getPointerElementType()->isStructTy())
            s << getParserRetrievalForNamedType(dummy, a.type->getPointerElementType(), isForGlobals);
        else{
            s << getParserRetrievalForNamedType(dummy, a.type, isForGlobals);
        }
    }
    return s.str();
}


std::string getTypedArgumentNames(std::vector<handarg> args){
    std::stringstream s;
    for(auto& a : args){
        if(a.passByVal && a.getType()->isPointerTy())
            s << getCTypeNameForLLVMType(a.getType()->getPointerElementType()) << ' ' << a.getName() << ',';
        else
            s << getCTypeNameForLLVMType(a.getType()) << ' ' << a.getName() << ',';
    }
    auto retstring = s.str();
    if(!retstring.empty())
        retstring.pop_back(); //remove last ,
    return retstring;
}

std::string getUntypedArgumentNames(std::vector<handarg> args){
    std::stringstream s;
    for(auto& a : args){
        s << a.getName()<< ',';
    }
    auto retstring = s.str();
    if(!retstring.empty())
        retstring.pop_back(); //remove last ,
    return retstring;
}

std::string getJsonObjectForScalar(Type* type){
    //TODO fix this for arrays
    auto typeString = std::string("PLACEHOLDER");
    for (auto & c: typeString) //didn't want to include boost::to_upper
        c = toupper(c);
    return "\"" + typeString + "_TOKEN" +  "\"" ;
}

std::string getJsonObjectForStruct(DefinedStruct ds, int depth = 2){
    std::stringstream s;
    s << "{" << std::endl;
    for(auto& mem : ds.getNamedMembers()){
        for(int i = 0; i < depth; i++)
            s << "\t";
        s << "\"" << mem.first << "\": ";
        if (mem.second->isStructTy()){
            s << getJsonObjectForStruct(getStructByLLVMType((StructType*)mem.second), depth+1);
        }
        else if(mem.second->isArrayTy()){
            s << "[";
            auto arrType = mem.second->getArrayElementType();
            for(int i = 0; i < mem.second->getArrayNumElements(); i++){
                if(arrType->isStructTy()){
                    s << getJsonObjectForStruct(getStructByLLVMType((StructType*)arrType));
                }
                else{
                    s << getJsonObjectForScalar(arrType);
                }
                if (i != mem.second->getArrayNumElements() -1){ // names should be unique inside a struct
                    s << ",";
                }
            }
            s << "]";
        }
        else{
            s << getJsonObjectForScalar((StructType*)mem.second);
        }
        if (mem.first != ds.getNamedMembers().back().first){ // names should be unique inside a struct
            s << ",";
        }
        s  << std::endl;
    }
    for(int i = 0; i < depth-1; i++)
        s << "\t";
    s << "}";
    return s.str();
}

std::string printRawLLVMType(Type* type){
    std::string type_str;
    llvm::raw_string_ostream rso(type_str);
    type->print(rso);
    return rso.str();
}

std::string getJsonInputTemplateText(std::vector<handarg> args, std::vector<handarg> globals){
    std::stringstream s;
    s << "{" << std::endl;

    std::vector<handarg> all_elements = globals;
    all_elements.insert(all_elements.end(), args.begin(), args.end());

    for(auto& arg: all_elements){
       s << "\t" << "\"" << arg.getName() << "\": ";
        if (arg.getType()->isStructTy()){
            s << getJsonObjectForStruct(getStructByLLVMType((StructType*)arg.getType()));
        }
        else if (arg.getType()->isPointerTy() && arg.getType()->getPointerElementType()->isStructTy()){
            s << getJsonObjectForStruct(getStructByLLVMType((StructType*)arg.getType()->getPointerElementType()));
        }
        else if(arg.getType()->isArrayTy()){
            s << "ARRAY_HERE";
        }
        else{
            s << getJsonObjectForScalar(arg.getType());
        }
        // check if we are at the end of the iterator and skip trailing ','
        // please let me know if you know something more elegant
        if (&arg != &*all_elements.rbegin()){
            s << "," << std::endl;
        }

    }

    s << std::endl << "}";
    return s.str();
}

int main(int argc, char** argv){
    // init llvm and open module
    InitLLVM X(argc, argv);
    ExitOnErr.setBanner(std::string(argv[0]) + ": error: ");
    LLVMContext Context;
    cl::ParseCommandLineOptions(argc, argv, "llvm .bc -> .ll disassembler\n");
    std::unique_ptr<MemoryBuffer> MB = ExitOnErr(errorOrToExpected(MemoryBuffer::getFileOrSTDIN(InputFilename)));
    BitcodeFileContents IF = ExitOnErr(llvm::getBitcodeFileContents(*MB));
    std::cout << "Bitcode file contains this many modules " << IF.Mods.size() << std::endl;
    std::unique_ptr<Module> mod = ExitOnErr(IF.Mods[0].parseModule(Context));

    int generatedFunctionCounter = 0;
    for(auto& f : mod->functions()) {
        std::string funcName = f.getName().str();
        if(funcName.rfind("_Z",0) != std::string::npos){
            std::cout << "function mangled " << funcName << std::endl;
            continue; // mangeld c++ function
        }
        std::cout << "Found function: " << funcName << " pure: " << f.doesNotReadMemory() <<  " accepting guments: " << std::endl;


        std::vector<handarg> globals_of_func = extractGlobalValuesFromModule(mod);
        std::vector<handarg> args_of_func = extractArgumentsFromFunction(f);

        // the return type might be a struct be an anonymous struct that is not defined in the bitcode module
        Type* funcRetType = f.getReturnType();
        if(funcRetType->isStructTy())
            defineIfNeeded(funcRetType, true);

        GenerateCppFunctionHarness(funcName, funcRetType, args_of_func, globals_of_func);
        GenerateJsonInputTemplateFile(funcName, args_of_func, globals_of_func);
        generatedFunctionCounter++;
    }

    return generatedFunctionCounter;
}

void GenerateCppFunctionHarness(std::string &funcName, Type *funcRetType, std::vector<handarg> &args_of_func,
                                std::vector<handarg> &globals_of_func) {
    std::ofstream ofs;
    auto output_file = OUTPUT_DIR + "/" + funcName + ".cpp";
    if(!std::filesystem::exists(OUTPUT_DIR))
        std::filesystem::create_directory(OUTPUT_DIR);

    ofs.open(output_file, std::ofstream::out | std::ofstream::trunc);
    std::string setupFileString = getSetupFileText(funcName, funcRetType, args_of_func, globals_of_func);
    ofs << setupFileString;
    ofs.close();
}

std::vector<handarg> extractGlobalValuesFromModule(std::unique_ptr<Module> &mod) {
    std::vector<handarg> globals;
    for(auto& global : mod->global_values())
    {
        // TODO: Should figure out what checks actually should be in here
        // Check if global is global value or something else,
        // I remove function declarations here, but what if a global value is a function pointer?
        if(!global.getType()->isFunctionTy()  && !(global.getType()->isPointerTy() && global.getType()->getPointerElementType()->isFunctionTy())  && global.isDSOLocal()){
            globals.push_back(handarg(global.getName(), 0 , global.getType()->getPointerElementType(), false));
        }
    }
    return globals;
}

void GenerateJsonInputTemplateFile(const std::string &funcName, std::vector<handarg> &args_of_func,
                                   std::vector<handarg> &globals) {
    std::ofstream ofsj;
    auto output_file_json = OUTPUT_DIR + "/" + funcName + ".json";
    if(!std::filesystem::exists(OUTPUT_DIR))
        std::filesystem::create_directory(OUTPUT_DIR);

    ofsj.open(output_file_json, std::ofstream::out | std::ofstream::trunc);
    ofsj << getJsonInputTemplateText(args_of_func, globals);
    ofsj.close();
}

std::vector<handarg> extractArgumentsFromFunction(Function &f) {
    std::vector<handarg> args;
    int argcounter = 0;
    for(auto& arg : f.args()){
        std::string argName = "";
        if(arg.hasName())
            argName = arg.getName();
        else
            argName = "e_" + std::to_string(argcounter);

        // TODO: Make sure that these names don't conflict with globals
        std::cout << "Name: " << argName << " type: " << printRawLLVMType(arg.getType()) << std::endl;
        args.push_back(handarg(argName, arg.getArgNo(), arg.getType(), arg.hasByValAttr()));
        argcounter++;
    }
    return args;
}
std::string getJsonOutputForType(std::vector<std::string> prefixes, Type* type){
    std::stringstream s;
    if(type->isPointerTy()){
        //TODO: Should this be casted to anything to ensure proper output format?
        s << "output_json" << joinStrings(prefixes, GENERATE_FORMAT_JSON_ARRAY_ADDRESSING)
            << " = " << joinStrings(prefixes, GENERATE_FORMAT_CPP_ADDRESSING) << ";" << std::endl;
    }
    else if(type->isStructTy()){
        auto definedStruct = getStructByLLVMType((StructType*)type);
        for(auto& mem : definedStruct.getNamedMembers()){
            std::vector<std::string> member_prefixes;
            member_prefixes.push_back(mem.first);
            s << getJsonOutputForType(member_prefixes, mem.second) << std::endl;
        }
    }
    else{
        s << "output_json" << joinStrings(prefixes, GENERATE_FORMAT_JSON_ARRAY_ADDRESSING)
          << " = " << joinStrings(prefixes, GENERATE_FORMAT_CPP_ADDRESSING) << ";" << std::endl;
    }
    return s.str();
}


// assume output_var_name contains the return value already set
std::string getJsonOutputText(std::string output_var_name, Type* retType){
    std::stringstream s;
    s << "nlohmann::json output_json;" << std::endl;
    std::vector<std::string> prefixes;
    prefixes.push_back(output_var_name);
    s << getJsonOutputForType(prefixes, retType);
    return s.str();
}


/*
 * The format of the generated file is:
 * header includes
 * struct declarations
 * test_function declaration
 * json struct mappings
 * helper functions used in generated text
 * global variable setup (including the parser)
 * setupParser Function
 * callFunction Function
 */
std::string getSetupFileText(std::string functionName, Type *funcRetType, std::vector<handarg> &args_of_func,
                             std::vector<handarg> &globals) {
    std::stringstream setupfilestream;

    // INCLUDES HEADERS
    setupfilestream << "#include \"skelmain.hpp\" " << std::endl;
    setupfilestream << "#include <nlohmann/json.hpp> // header only lib" << std::endl << std::endl;


    // DECLARE STRUCTS

    defineStructs(globals);
    defineStructs(args_of_func);

    setupfilestream << getStructDefinitions() << std::endl;

    // DECLARATION TEST FUNCTION
    // the LLVM type name differs from how we define it in our generated source code
    std::string rettype = getCTypeNameForLLVMType(funcRetType);
    if(funcRetType->isStructTy())
        rettype = getStructByLLVMType((StructType*)funcRetType).definedCStructName;
    std::string functionSignature = rettype + " " +  functionName + "(" + getTypedArgumentNames(args_of_func) + ");";
    setupfilestream << "extern \"C\" " + functionSignature << std::endl;

    // DECLARE GLOBALS
    setupfilestream << "argparse::ArgumentParser parser;" << std::endl << std::endl;
    setupfilestream << "// globals from module" << std::endl << getDefineGlobalsText(globals)  << std::endl << std::endl;


    // DEFINE SETUPPARSER
    setupfilestream << "void setupParser() { " << std::endl
            << "std::stringstream s;"
            << "s << \"Test program for: " << functionSignature << "\" << std::endl << R\"\"\"("  << getStructDefinitions() << ")\"\"\";" << std::endl
            << "\tparser = argparse::ArgumentParser(s.str());" << std::endl
            << "} " << std::endl << std::endl;

    // DEFINE CALLFUNCTION
    setupfilestream << "void callFunction() { " << std::endl
    << "\t\t" << "std::string inputfile = parser.get<std::string>(\"-i\");" << std::endl
    << "\t\t" << "std::ifstream i(inputfile);\n"
    "\t\tnlohmann::json j;\n"
    "\t\ti >> j;\n"
    << getParserRetrievalText(globals, true)
    << getParserRetrievalText(args_of_func, false);

    auto output_var_name = "output";
    setupfilestream << "auto " << output_var_name << " = " <<  functionName << "(" << getUntypedArgumentNames(args_of_func) << ");" << std::endl;
    setupfilestream <<  getJsonOutputText(output_var_name, funcRetType);

    setupfilestream << "\t\t" << "std::cout << output_json << std::endl;" << std::endl;


    setupfilestream << "} " << std::endl;
    std::string setupFileString = setupfilestream.str();
    return setupFileString;
}
