#include "clang/Driver/Options.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <set>
#include <sstream>
#include <vector>
#include <map>
#include <tuple>
#include <iostream>

using namespace std;
using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace llvm;

/** Options **/
static cl::OptionCategory CPP2CCategory("CPP2C options");
//static std::unique_ptr<opt::OptTable> Options(getDriverOptTable());
static opt::OptTable Options = getDriverOptTable();
static cl::opt<std::string> OutputFilename("o", cl::desc(Options.getOptionHelpText((options::OPT_o))));

/** Classes to be mapped to C **/
struct OutputStreams {
    string headerString;
    string bodyString;

    llvm::raw_string_ostream HeaderOS;
    llvm::raw_string_ostream BodyOS;

    OutputStreams() : headerString(""), bodyString(""), HeaderOS(headerString), BodyOS(bodyString) {};
};

vector<string> ClassList = {"RationalTime"};
vector<string> EnumList = {"IsDropFrameRate"};
string namespaceStr = "opentime::";
map<string, int> funcList;

/** Matchers **/

/** Handlers **/
class classMatchHandler : public MatchFinder::MatchCallback {
public:
    classMatchHandler(OutputStreams &os) : OS(os) {}

    tuple<string, string, bool, bool> determineCType(const QualType &qt) {

        string CType = "";
        string CastType = ""; //whether this should be casted or not
        bool isPointer = false;
        bool shouldReturn = true;

        //if it is builtint type use it as is
        if (qt->isBuiltinType() || (qt->isPointerType() && qt->getPointeeType()->isBuiltinType())) {
            CType = qt.getAsString();
            if (qt->isVoidType())
                shouldReturn = false;
            //if it is a CXXrecordDecl then return a pointer to WName*
        } else if (qt->isRecordType()) {
            const CXXRecordDecl *crd = qt->getAsCXXRecordDecl();
            string recordName = crd->getNameAsString();
            CType = recordName + "*";
            CastType = recordName + "*";

        } else if ((qt->isReferenceType() || qt->isPointerType()) && qt->getPointeeType()->isRecordType()) {
            isPointer = true; //to properly differentiate among cast types
            const CXXRecordDecl *crd = qt->getPointeeType()->getAsCXXRecordDecl();
            string recordName = crd->getNameAsString();
            if (std::find(ClassList.begin(), ClassList.end(), recordName) != ClassList.end()) {
                CType = recordName + "*";
                CastType = recordName + "*";
            } else {
                CType = recordName + "*";
            }

        }
        if(CType == "basic_string*") CType = "const char*";
        if(CastType == "basic_string*") CastType = "const char*";
        return make_tuple(CType, CastType, isPointer, shouldReturn);

    }

    virtual void run(const MatchFinder::MatchResult &Result) {
        if(const EnumDecl *eDecl = Result.Nodes.getNodeAs<EnumDecl>("enumDecl")){

            OS.HeaderOS << "\n" << "enum " << eDecl->getNameAsString() << "{\n";

            for(auto it = eDecl->enumerator_begin(); it!=eDecl->enumerator_end(); it++){
                OS.HeaderOS << "    " << it->getNameAsString() << " = " << it->getInitVal().getExtValue() << ",\n";
            }
            OS.HeaderOS << "};\n";
        }
        if (const CXXMethodDecl *cmd = Result.Nodes.getNodeAs<CXXMethodDecl>("publicMethodDecl")) {
            string methodName = "";
            string className = cmd->getParent()->getDeclName().getAsString();
            string returnType = "";
            string returnCast = "";
            bool shouldReturn, isPointer;
            bool returnSelfType = false;
            string self = className + "* self";
            string separator = ", ";
            string bodyEnd = "";

            std::stringstream functionBody;

            //ignore operator overloadings
            if (cmd->isOverloadedOperator())
                return;

            //constructor
            if (const CXXConstructorDecl *ccd = dyn_cast<CXXConstructorDecl>(cmd)) {
                if (ccd->isCopyConstructor() || ccd->isMoveConstructor()) return;
                methodName = "_create";
                returnType = className + "*";
                self = "";
                separator = ", ";
                functionBody << "return reinterpret_cast<" << returnType << ">( new " << namespaceStr + className
                             << "(";
                bodyEnd += "))";
            } else if (isa<CXXDestructorDecl>(cmd)) {
                methodName = "_destroy";
                returnType = "void";
                functionBody << " delete reinterpret_cast<" << namespaceStr << className << "*>(self)";
            } else {
                methodName = "_" + cmd->getNameAsString();
                const QualType qt = cmd->getReturnType();
                std::tie(returnType, returnCast, isPointer, shouldReturn) = determineCType(qt);
//                cout<<methodName<<" "<<returnType<< " " << returnCast <<endl;
                if(returnType == className+"*"){
                    returnSelfType = true;
                    functionBody << namespaceStr << className << " obj = ";
                    functionBody << "reinterpret_cast<" << namespaceStr << className << "*>(self)->"
                                 << cmd->getNameAsString() << "(";
                }
                else{
                    //should this function return?
                    if (shouldReturn)
                        functionBody << "return ";

                    if (returnCast != "") {
                        functionBody << "reinterpret_cast<" << returnType << ">(";
                        bodyEnd += ")";
                    }

                    //if Static call it properly
                    if (cmd->isStatic())
                        functionBody << namespaceStr << className << "::" << cmd->getNameAsString() << "(";
                        //if not  use the passed object to call the method
                    else
                        functionBody << "reinterpret_cast<" << namespaceStr << className << "*>(self)->"
                                     << cmd->getNameAsString() << "(";

                    bodyEnd += ")";
                }


            }


            std::stringstream funcname;
            funcname << returnType << " " << className << methodName;

            auto it = funcList.find(funcname.str());

            if (it != funcList.end()) {
                it->second++;
                funcname << "_" << it->second;
            } else {
                funcList[funcname.str()] = 0;
            }

            funcname << "(" << self;

            for (unsigned int i = 0; i < cmd->getNumParams(); i++) {
                const QualType qt = cmd->parameters()[i]->getType();
//                cout<<methodName<<" "<<qt.getAsString()<<endl;
                std::tie(returnType, returnCast, isPointer, shouldReturn) = determineCType(qt);
                if(i == 0 && self.empty()){
                    funcname << returnType << " ";
                }
                else {
                    funcname << separator << returnType << " ";
                }
                funcname << cmd->parameters()[i]->getQualifiedNameAsString() << "";

                if (i != 0)
                    functionBody << separator;
                if (returnCast == "")
                    functionBody << cmd->parameters()[i]->getQualifiedNameAsString();
                else {
                    if (!isPointer)
                        functionBody << "*";
                    functionBody << "reinterpret_cast<" << namespaceStr<<returnCast << ">("
                                 << cmd->parameters()[i]->getQualifiedNameAsString() << ")";

                }

                string separator = ", ";

            }
            funcname << ")";
            if(returnSelfType){
                functionBody << ");\n    ";
                functionBody << "return reinterpret_cast<" << className << "*>(new "
                             << namespaceStr << className << "(obj))";
            }

            OS.HeaderOS << funcname.str() << ";\n";

            OS.BodyOS << funcname.str() << "{\n    ";
            OS.BodyOS << functionBody.str();
            OS.BodyOS << bodyEnd << "; \n}\n";
        }
    }

    virtual void onEndOfTranslationUnit() {}

private:
    OutputStreams &OS;

};


/****************** /Member Functions *******************************/
// Implementation of the ASTConsumer interface for reading an AST produced
// by the Clang parser. It registers a couple of matchers and runs them on
// the AST.
class MyASTConsumer : public ASTConsumer {
public:
    MyASTConsumer(OutputStreams &os) : OS(os),
                                       HandlerForClassMatcher(os) {
        // Add a simple matcher for finding 'if' statements.
        for (string &enumName : EnumList) {
            DeclarationMatcher enumMatcher = enumDecl(hasName(enumName)).bind("enumDecl");
            Matcher.addMatcher(enumMatcher, &HandlerForClassMatcher);
        }

        for (string &className : ClassList) {
            OS.HeaderOS << "struct      " << className << "; \n"
                           "typedef     struct " << className << " " << className
                        << ";\n";
            //oss.push_back(std::move(os))

            DeclarationMatcher classMatcher = cxxMethodDecl(isPublic(), ofClass(hasName(className))).bind(
                    "publicMethodDecl");
            Matcher.addMatcher(classMatcher, &HandlerForClassMatcher);
        }

    }

    void HandleTranslationUnit(ASTContext &Context) override {
        // Run the matchers when we have the whole TU parsed.
        Matcher.matchAST(Context);
    }

private:
    OutputStreams &OS;
    classMatchHandler HandlerForClassMatcher;

    MatchFinder Matcher;
};

// For each source file provided to the tool, a new FrontendAction is created.
class MyFrontendAction : public ASTFrontendAction {
public:
    MyFrontendAction() {
        OS.HeaderOS << "#ifdef __cplusplus\n"
                       "extern \"C\"{\n"
                       "#endif\n"
                       "#include <stdbool.h>\n";
        OS.BodyOS << "#ifdef __cplusplus\n"
                     "extern \"C\"{\n"
                     "#endif\n";

    }

    void EndSourceFileAction() override {

        StringRef headerFile("cwrapper.h");
        StringRef bodyFile("cwrapper.cpp");

        // Open the output file
        std::error_code EC;
        llvm::raw_fd_ostream HOS(headerFile, EC, llvm::sys::fs::F_None);
        if (EC) {
            llvm::errs() << "while opening '" << headerFile << "': "
                         << EC.message() << '\n';
            exit(1);
        }
        llvm::raw_fd_ostream BOS(bodyFile, EC, llvm::sys::fs::F_None);
        if (EC) {
            llvm::errs() << "while opening '" << bodyFile << "': "
                         << EC.message() << '\n';
            exit(1);
        }


        OS.HeaderOS << "#ifdef __cplusplus\n"
                       "}\n"
                       "#endif\n";

        OS.BodyOS << "#ifdef __cplusplus\n"
                     "}\n"
                     "#endif\n";

        OS.HeaderOS.flush();
        OS.BodyOS.flush();
        HOS << OS.headerString << "\n";
        BOS << OS.bodyString << "\n";

    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override {

        return make_unique<MyASTConsumer>(OS);
    }

private:
    OutputStreams OS;
};

int main(int argc, const char **argv) {
    vector<const char*> cla;
    for(auto& s : std::vector<const char*>(argv,argv+argc)){
        cla.push_back(s);
    }
    cla.push_back("-I/usr/include/c++/7.5.0");
    cla.push_back("-I/usr/lib/gcc/x86_64-linux-gnu/7/include/");
    cla.push_back("-I/usr/include/x86_64-linux-gnu/c++/7.5.0");
    cla.push_back("-std=c++11");
    const char **claptr1 = &cla[0];
    int argcnt = cla.size();
    // parse the command-line args passed to your code
    CommonOptionsParser op(argcnt, claptr1, CPP2CCategory);
    // create a new Clang Tool instance (a LibTooling environment)
    ClangTool Tool(op.getCompilations(), op.getSourcePathList());
//#define STRINGIFY(x) #x
//#define TOSTRING(x) STRINGIFY(x)
//#define AT __FILE__ ":" TOSTRING(__LINE__)
//#ifdef LIB_PATH
//    string path = TOSTRING(LIB_PATH);
//    cout<<path<<endl;
//#endif
    // run the Clang Tool, creating a new FrontendAction
    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
