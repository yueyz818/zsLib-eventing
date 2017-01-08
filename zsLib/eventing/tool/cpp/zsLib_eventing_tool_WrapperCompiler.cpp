/*

Copyright (c) 2016, Robin Raymond
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the FreeBSD Project.

*/

#include <zsLib/eventing/tool/internal/zsLib_eventing_tool_WrapperCompiler.h>
#include <zsLib/eventing/tool/internal/zsLib_eventing_tool_Helper.h>

#include <zsLib/eventing/tool/OutputStream.h>

#include <zsLib/eventing/IHelper.h>
#include <zsLib/eventing/IHasher.h>
#include <zsLib/eventing/IEventingTypes.h>

#include <zsLib/Exception.h>
#include <zsLib/Numeric.h>

#include <sstream>
#include <list>
#include <set>

#define ZS_WRAPPER_COMPILER_DIRECTIVE_EXCLUSIZE "EXCLUSIVE"

namespace zsLib { namespace eventing { namespace tool { ZS_DECLARE_SUBSYSTEM(zsLib_eventing_tool) } } }

namespace zsLib
{
  namespace eventing
  {
    ZS_DECLARE_TYPEDEF_PTR(IWrapperTypes::Project, Project);

    namespace tool
    {
      ZS_DECLARE_TYPEDEF_PTR(eventing::IHelper, UseHelper);
      ZS_DECLARE_TYPEDEF_PTR(eventing::IHasher, UseHasher);
      typedef std::set<String> HashSet;

      namespace internal
      {
        ZS_DECLARE_TYPEDEF_PTR(WrapperCompiler::Token, Token);
        typedef WrapperCompiler::TokenList TokenList;

        //-----------------------------------------------------------------------
        //-----------------------------------------------------------------------
        //-----------------------------------------------------------------------
        //-----------------------------------------------------------------------
        #pragma mark
        #pragma mark Helpers
        #pragma mark

        //---------------------------------------------------------------------
        static void skipPreprocessor(
                                     const char * &p,
                                     ULONG &ioLineCount
                                     )
        {
          const char *startPos = p;
          
          while (true)
          {
            Helper::skipToEOL(p);

            // see if this preprocessor statement is multi-line
            while (p != startPos)
            {
              --p;
              if (('\n' == *p) ||
                  ('\r' == *p)) {
                break;
              }

              if (isspace(*p)) continue;

              if ('\\' == *p) {
                Helper::skipToEOL(p);
                if (Helper::skipEOL(p, &ioLineCount)) goto next_line;
              }
              Helper::skipToEOL(p);
              Helper::skipEOL(p, &ioLineCount);
              return;
            }

          next_line:
            {
            }
          }
        }
        
        //---------------------------------------------------------------------
        static TokenPtr getCPPDirectiveToken(
                                             const char *p,
                                             ULONG &ioLineCount
                                             )
        {
          if ('/' != *p) return TokenPtr();
          if ('/' != *(p+1)) return TokenPtr();
          if ('!' != *(p+2)) return TokenPtr();

          p += 3;

          const char *start = p;
          Helper::skipToEOL(p);

          String str(p, static_cast<size_t>(p - start));

          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Directive;
          result->mToken = str;
          result->mLineCount = ioLineCount;

          Helper::skipEOL(p, &ioLineCount);

          return result;
        }
        
        //---------------------------------------------------------------------
        static TokenPtr getCPPDocToken(
                                       const char *p,
                                       ULONG &ioLineCount
                                       )
        {
          if ('/' != *p) return TokenPtr();
          if ('/' != *(p+1)) return TokenPtr();
          if ('/' != *(p+2)) return TokenPtr();
          
          p += 3;

          const char *start = p;
          Helper::skipToEOL(p);
          
          String str(p, static_cast<size_t>(p - start));
          
          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Documentation;
          result->mToken = str;
          result->mLineCount = ioLineCount;

          Helper::skipEOL(p, &ioLineCount);

          return result;
        }
        
        //---------------------------------------------------------------------
        static TokenPtr getQuoteToken(
                                      const char * &p,
                                      ULONG &ioLineCount
                                      )
        {
          ULONG currentLine = ioLineCount;
          
          const char *start = p;
          if (!Helper::skipQuote(p, &ioLineCount)) return TokenPtr();

          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Quote;
          result->mToken = String(start, static_cast<size_t>(p - start));
          result->mLineCount = currentLine;
          return result;
        }

        //---------------------------------------------------------------------
        static TokenPtr getCharToken(
                                     const char * &p,
                                     ULONG &ioLineCount
                                     ) throw (FailureWithLine)
        {
          ULONG currentLine = ioLineCount;
          
          const char *start = p;
          if ('\'' != *p) return TokenPtr();
          
          ++p;
          if ('\\' == *p) {
            Helper::decodeCEscape(p, ioLineCount);
          } else {
            ++p;
          }
          if ('\'' != *p) return TokenPtr();
          ++p;

          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Char;
          result->mToken = String(start, static_cast<size_t>(p - start));
          result->mLineCount = currentLine;
          return result;
        }

        //---------------------------------------------------------------------
        static TokenPtr getNumberToken(
                                       const char * &p,
                                       ULONG lineCount
                                       )
        {
          const char *start = p;
          
          bool foundNegative = false;
          bool foundDot = false;
          bool foundExponent = false;

          if ('-' == *start) {
            foundNegative = true;
            ++start;
            Helper::skipWhitespaceExceptEOL(start);
          }

          if (!isnumber(*start)) return TokenPtr();

          p = start;
          
          int base = 10;
          
          if ('0' == *p) {
            switch (*(p+1)) {
              case 'x':
              case 'X': {
                base = 16;
                p += 2;
                break;
              }
              case 'b':
              case 'B': {
                base = 2;
                p += 2;
                break;
              }
              case '.': {
                break;
              }
              case '0':
              case '1':
              case '2':
              case '3':
              case '4':
              case '5':
              case '6':
              case '7': {
                base = 8;
                ++p;
                break;
              }
            }
          }

          while ('\0' != *p)
          {
            switch (*p)
            {
              case '.': {
                if (10 != base) goto check_exponent;
                ++p;
                foundDot = true;
                continue;
              }
              case '0':
              case '1': {
                ++p;
                continue;
              }
              case '2':
              case '3':
              case '4':
              case '5':
              case '6':
              case '7': {
                if (base >= 8) {
                  ++p;
                  continue;
                }
                goto check_exponent;
              }
              case '8':
              case '9': {
                if (base >= 10) {
                  ++p;
                  continue;
                }
                goto check_exponent;
              }
              case 'a':
              case 'A':
              case 'b':
              case 'B':
              case 'c':
              case 'C':
              case 'd':
              case 'D':
              case 'e':
              case 'E':
              case 'f':
              case 'F': {
                if (base >= 10) {
                  ++p;
                  continue;
                }
                goto check_exponent;
              }
              default: goto check_exponent;
            }
          }
          
        check_exponent:
          {
            const char *exponentStart = p;
            
            if (('e' != *p) &&
                ('E' != *p)) {
              goto check_postfix;
            }
            if (10 != base) goto check_postfix;

            foundExponent = true;
            ++p;
            
            bool foundExponentNumber = false;
            if (('-' == *p) ||
                ('+' == *p)) {
              ++p;
            }

            while (isnumber(*p)) {
              foundExponentNumber = true;
              ++p;
            }

            if (!foundExponentNumber) {
              // the 'e' does not belong to the number
              p = exponentStart;
              goto done;
            }
          }
          
        check_postfix:
          {
            const char *postFixStart = p;
            
            bool foundUnsigned = false;
            bool foundFloat = false;
            size_t foundLongs = 0;
            bool lastWasLong = false;
            
            while (true)
            {
              switch (*p) {
                case 'u':
                case 'U':
                {
                  if (foundUnsigned) goto invalid_postfix;
                  if (foundFloat) goto invalid_postfix;
                  foundUnsigned = true;
                  goto not_long;
                }
                case 'l':
                case 'L':
                {
                  if (foundLongs > 0) {
                    if (foundFloat) goto invalid_postfix;
                    if (!lastWasLong) goto invalid_postfix;
                  }
                  ++foundLongs;
                  if (foundLongs > 2) goto invalid_postfix;
                  ++p;
                  lastWasLong = true;
                  continue;
                }
                case 'f':
                case 'F':
                {
                  if (10 != base) goto invalid_postfix;
                  if (foundUnsigned) goto invalid_postfix;
                  if (foundLongs > 1) goto invalid_postfix;
                  if (foundFloat) goto invalid_postfix;
                  foundFloat = true;
                  goto not_long;
                }
                default:
                {
                  goto done;
                }
              }
              
            not_long:
              {
                ++p;
                lastWasLong = false;
                continue;
              }
              
            invalid_postfix:
              {
                p = postFixStart;
                goto done;
              }
            }
          }
          
        done:
          {
          }
          
          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Number;
          result->mToken = String(start, static_cast<size_t>(p - start));
          if (foundNegative) {
            result->mToken = String("-") + result->mToken;
          }
          result->mLineCount = lineCount;
          return result;
        }
        
        //---------------------------------------------------------------------
        static TokenPtr getIdentifierToken(
                                           const char * &p,
                                           ULONG lineCount
                                           )
        {
          if ((!isalpha(*p)) &&
              ('_' != *p)) return TokenPtr();
          
          const char *start = p;
          
          while ((isalnum(*p)) ||
                 ('_' == *p)) {
            ++p;
          }
          
          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Identifier;
          result->mToken = String(start, static_cast<size_t>(p - start));
          result->mLineCount = lineCount;
          return result;
        }
        
        //---------------------------------------------------------------------
        static TokenPtr getBraceToken(
                                      const char * &p,
                                      ULONG lineCount
                                      )
        {
          WrapperCompiler::TokenTypes type {WrapperCompiler::TokenType_First};
          
          switch (*p) {
            case '{':
            case '}': type = WrapperCompiler::TokenType_CurlyBrace; break;
            case '(':
            case ')': type = WrapperCompiler::TokenType_Brace; break;
            case '[':
            case ']': type = WrapperCompiler::TokenType_SquareBrace; break;
            default:  return TokenPtr();
          }

          auto result = make_shared<Token>();
          result->mTokenType = type;
          result->mToken = String(p, static_cast<size_t>(1));
          result->mLineCount = lineCount;

          ++p;

          return result;
        }

        //---------------------------------------------------------------------
        static TokenPtr getOperatorToken(
                                         const char * &p,
                                         ULONG lineCount
                                         )
        {
          static const char *operators[] =
          {
            ";",
            "::",
            "++",
            "--",
            ".",
            "->",
            "~",
            "!",
            "+",
            "-",
            "&",
            "*",
            ".*",
            "->*",
            "*",
            "/",
            "%",
            "<<",
            ">>",
            "<",
            ">",
            ">=",
            "<=",
            "==",
            "!=",
            "^",
            "|",
            "&&",
            "||",
            "=",
            "*=",
            "/=",
            "%=",
            "+=",
            "-=",
            ">>=",
            "<<=",
            "&=",
            "^=",
            "|=",
            "?",
            ":",
            ",",
            NULL
          };
          
          String valid;
          String test;

          while ('\0' != *p) {
            test = valid;
            test += String(p, static_cast<size_t>(1));
            
            for (int index = 0; NULL != operators[index]; ++index)
            {
              if (test == operators[index]) goto next;
            }
            goto done;
            
          next:
            {
              valid = test;
              ++p;
            }
          }

        done:
          {
          }
          
          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Operator;
          if (";" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_SemiColon;
          } else if ("::" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_ScopeOperator;
          } else if ("," == valid) {
            result->mTokenType = WrapperCompiler::TokenType_CommaOperator;
          } else if (":" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_ColonOperator;
          } else if ("=" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_EqualsOperator;
          } else if ("*" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_PointerOperator;
          } else if ("&" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_AddressOperator;
          } else if ("<" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_AngleBrace;
          } else if (">" == valid) {
            result->mTokenType = WrapperCompiler::TokenType_AngleBrace;
          }

          result->mToken = valid;
          result->mLineCount = lineCount;
          return result;
        }
        
        //---------------------------------------------------------------------
        static TokenPtr getUnknownToken(
                                        const char * &p,
                                        ULONG lineCount
                                        )
        {
          if (!p) return TokenPtr();
          
          if ('\0' == *p) return TokenPtr();
          
          auto result = make_shared<Token>();
          result->mTokenType = WrapperCompiler::TokenType_Unknown;
          result->mToken = String(p, static_cast<size_t>(1));
          result->mLineCount = lineCount;
          ++p;
          return result;
        }

        //---------------------------------------------------------------------
        static TokenPtr getNextToken(
                                     const char * &p,
                                     bool &ioStartOfLine,
                                     ULONG &ioLineCount
                                     )
        {
          if (!p) return TokenPtr();
          
          while ('\0' != *p)
          {
            ULONG activeLine = ioLineCount;
            
            if (Helper::skipWhitespaceExceptEOL(p)) continue;
            if (Helper::skipEOL(p, &ioLineCount)) {
              ioStartOfLine = true;
              continue;
            }
            
            if (ioStartOfLine) {
              if ('#' == *p) {
                skipPreprocessor(p, ioLineCount);
                ioStartOfLine = true;
                continue;
              }
            }

            if (Helper::skipCComments(p, &ioLineCount)) {
              if (activeLine != ioLineCount) ioStartOfLine = true;
              continue;
            }

            {
              auto result = getCPPDirectiveToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = true;
                return result;
              }
            }

            {
              auto result = getCPPDocToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = true;
                return result;
              }
            }

            if (Helper::skipCPPComments(p)) {
              Helper::skipEOL(p, &ioLineCount);
              ioStartOfLine = true;
              continue;
            }
            
            {
              auto result = getQuoteToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = false;
                return result;
              }
            }
            
            {
              auto result = getNumberToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = false;
                return result;
              }
            }
            
            {
              auto result = getIdentifierToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = false;
                return result;
              }
            }
            
            {
              auto result = getBraceToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = false;
                return result;
              }
            }

            {
              auto result = getOperatorToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = false;
                return result;
              }
            }
            
            {
              auto result = getUnknownToken(p, ioLineCount);
              if (result) {
                ioStartOfLine = false;
                return result;
              }
            }
          }

          return TokenPtr();
        }

        //---------------------------------------------------------------------
        void tokenize(
                      const char *p,
                      TokenList &outTokens,
                      ULONG startLineNumber = 1
                      )
        {
          bool startOfLine = true;
          ULONG lineCount = startLineNumber;
          
          while (true)
          {
            auto token = getNextToken(p, startOfLine, lineCount);
            if (!token) break;

            outTokens.push_back(token);
          }
        }
        
        //---------------------------------------------------------------------
        void replaceAliases(
                            TokenList &ioTokens,
                            const IEventingTypes::AliasMap &aliases
                            )
        {
          for (auto iter_doNotUse = ioTokens.begin(); iter_doNotUse != ioTokens.end(); )
          {
            auto current = iter_doNotUse;
            ++iter_doNotUse;
            
            auto token = (*current);
            auto found = aliases.find(token->mToken);
            if (found == aliases.end()) continue;

            TokenList replacementTokens;
            tokenize((*found).second.c_str(), replacementTokens, token->mLineCount);
            
            for (auto iterReplace = replacementTokens.rbegin(); iterReplace != replacementTokens.rend(); ++iterReplace)
            {
              auto replaceToken = (*iterReplace);
              ioTokens.insert(current, replaceToken);
            }

            ioTokens.erase(current);
          }
        }

        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark WrapperCompiler
        #pragma mark

        //---------------------------------------------------------------------
        bool WrapperCompiler::Token::isBrace() const
        {
          switch (mTokenType) {
            case TokenType_Brace:
            case TokenType_CurlyBrace:
            case TokenType_SquareBrace:
            case TokenType_AngleBrace:    return true;
            default:                      break;
          }
          return false;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::Token::isOpenBrace() const
        {
          switch (mTokenType) {
            case TokenType_Brace:         return "(" == mToken;
            case TokenType_CurlyBrace:    return "{" == mToken;
            case TokenType_SquareBrace:   return "[" == mToken;
            case TokenType_AngleBrace:    return "<" == mToken;
            default:                      break;
          }
          return false;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::Token::isCloseBrace() const
        {
          switch (mTokenType) {
            case TokenType_Brace:         return ")" == mToken;
            case TokenType_CurlyBrace:    return "}" == mToken;
            case TokenType_SquareBrace:   return "]" == mToken;
            case TokenType_AngleBrace:    return ">" == mToken;
            default:                      break;
          }
          return false;
        }

        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark WrapperCompiler
        #pragma mark

        //---------------------------------------------------------------------
        WrapperCompiler::WrapperCompiler(
                                           const make_private &,
                                           const Config &config
                                           ) :
          mConfig(config)
        {
        }

        //---------------------------------------------------------------------
        WrapperCompiler::~WrapperCompiler()
        {
        }

        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark WrapperCompiler => ICompiler
        #pragma mark

        //---------------------------------------------------------------------
        WrapperCompilerPtr WrapperCompiler::create(const Config &config)
        {
          WrapperCompilerPtr pThis(std::make_shared<WrapperCompiler>(make_private{}, config));
          pThis->mThisWeak = pThis;
          return pThis;
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::process() throw (Failure, FailureWithLine)
        {
          outputSkeleton();
          read();
          validate();
          if ((mConfig.mOutputName.hasData()) &&
              (mConfig.mProject)) {
          }
        }

        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        //---------------------------------------------------------------------
        #pragma mark
        #pragma mark WrapperCompiler => (internal)
        #pragma mark

        //---------------------------------------------------------------------
        void WrapperCompiler::outputSkeleton()
        {
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::read() throw (Failure, FailureWithLine)
        {
          HashSet processedHashes;

          ProjectPtr &project = mConfig.mProject;
          
          SecureByteBlockPtr configRaw;

          try {
            configRaw = UseHelper::loadFile(mConfig.mConfigFile);
          } catch (const StdError &e) {
            ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_FILE_FAILED_TO_LOAD, String("Failed to load main configuration file: ") + mConfig.mConfigFile + ", error=" + string(e.result()) + ", reason=" + e.message());
          }
          if (!configRaw) {
            ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_FILE_FAILED_TO_LOAD, String("Failed to load main configuration file: ") + mConfig.mConfigFile);
          }
          processedHashes.insert(UseHasher::hashAsString(configRaw));
          auto rootEl = UseHelper::read(configRaw);

          try {
            project = Project::create(rootEl);
          } catch (const InvalidContent &e) {
            ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_INVALID_CONTENT, "Failed to parse main configuration: " + e.message());
          }

          StringList sources = mConfig.mSourceFiles;
          mConfig.mSourceFiles.clear();

          ElementPtr sourcesEl = rootEl->findFirstChildElement("includes");
          if (sourcesEl) {
            ElementPtr sourceEl = sourcesEl->findFirstChildElement("include");
            while (sourceEl) {
              auto source = UseHelper::getElementTextAndDecode(sourceEl);

              if (source.hasData()) {
                mConfig.mSourceFiles.push_back(source);
              }
              sourceEl = sourceEl->findNextSiblingElement("include");
            }
          }

          // put back the original configuration files
          for (auto iter = sources.begin(); iter != sources.end(); ++iter) {
            mConfig.mSourceFiles.push_back(*iter);
          }

          ElementPtr includesEl = rootEl->findFirstChildElement("sources");
          if (includesEl) {
            ElementPtr includeEl = includesEl->findFirstChildElement("source");
            while (includeEl) {
              auto source = UseHelper::getElementTextAndDecode(includeEl);

              if (source.hasData()) {
                mConfig.mSourceFiles.push_back(source);
              }
              includeEl = includeEl->findNextSiblingElement("source");
            }
          }

          while (mConfig.mSourceFiles.size() > 0)
          {
            String fileName = mConfig.mSourceFiles.front();
            mConfig.mSourceFiles.pop_front();

            SecureByteBlockPtr file;
            try {
              file = UseHelper::loadFile(fileName);
            } catch (const StdError &e) {
              ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_FILE_FAILED_TO_LOAD, String("Failed to load main configuration file: ") + mConfig.mConfigFile + ", error=" + string(e.result()) + ", reason=" + e.message());
            }
            if (!file) {
              ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_FILE_FAILED_TO_LOAD, String("Failed to load file: ") + fileName);
            }
            auto hashResult = UseHasher::hashAsString(file);
            auto found = processedHashes.find(hashResult);
            if (found != processedHashes.end()) {
              tool::output() << "[Info] Duplicate file found thus ignoring: " << fileName << "\n";
              continue;
            }
            const char *fileAsStr = reinterpret_cast<const char *>(file->BytePtr());
            auto isJSON = Helper::isLikelyJSON(fileAsStr);

            if (isJSON) {
              try {
                tool::output() << "\n[Info] Reading JSON configuration: " << fileName << "\n\n";
                auto rootEl = UseHelper::read(file);
                if (!rootEl) {
                  ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_FILE_FAILED_TO_LOAD, String("Failed to load file as JSON: ") + fileName);
                }
                if (!project) {
                  project = Project::create(rootEl);
                } else {
                  project->parse(rootEl);
                }
              } catch (const InvalidContent &e) {
                ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_INVALID_CONTENT, "Failed to parse JSON configuration: " + e.message());
              }
              continue;
            }

            if (!project) {
              ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_INVALID_CONTENT, "Project configuration is missing!");
            }

            tool::output() << "\n[Info] Reading C/C++ source file: " << fileName << "\n\n";

            try {
              const char *pos = reinterpret_cast<const char *>(file->BytePtr());

              mTokenListStack = TokenListStack();

              pushTokens(make_shared<TokenList>());

              tokenize(pos, *getTokens());

              replaceAliases(*getTokens(), project->mAliases);

              if (!project->mGlobal) {
                project->mGlobal = Namespace::create(project);
              }

              parseNamespaceContents(project->mGlobal);

            } catch (const InvalidContent &e) {
              ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_INVALID_CONTENT, "Invalid content found: " + e.message());
            } catch (const InvalidContentWithLine &e) {
              ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, e.lineNumber(), "Invalid content found: " + e.message());
            }
          }
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::validate() throw (Failure)
        {
          auto &project = mConfig.mProject;
          if (!project) return;

//          if (project->mUniqueHash.isEmpty()) {
//            project->mUniqueHash = project->uniqueEventingHash();
//          }
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::parseNamespace(NamespacePtr parent) throw (FailureWithLine)
        {
          auto token = peekNextToken("namespace");
          if (TokenType_Identifier != token->mTokenType) return false;
          if ("namespace" != token->mToken) return false;

          extractNextToken("namespace");  // skip "namespace"

          token = extractNextToken("namespace");
          
          if (TokenType_Identifier != token->mTokenType) {
            ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, token->mLineCount, String("namespace missing identifier"));
          }
          
          String namespaceStr = token->mToken;

          token = extractNextToken("namespace");

          if ((TokenType_CurlyBrace != token->mTokenType) ||
              (token->isOpenBrace())) {
            ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, token->mLineCount, String("namespace expecting \"{\""));
          }

          NamespacePtr namespaceObj;

          {
            auto found = parent->mNamespaces.find(namespaceStr);
            if (found == parent->mNamespaces.end()) {
              namespaceObj = Namespace::create(parent);
              namespaceObj->mName = namespaceStr;
              namespaceObj->mDocumentation = getDocumentation();
              parent->mNamespaces[namespaceStr] = namespaceObj;
            } else {
              namespaceObj = (*found).second;
              mergeDocumentation(namespaceObj->mDocumentation);
            }
          }

          parseNamespaceContents(namespaceObj);

          token = extractNextToken("namespace");

          if ((TokenType_CurlyBrace != token->mTokenType) ||
              (token->isCloseBrace())) {
            ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, token->mLineCount, String("namespace expecting \"}\""));
          }

          return true;
        }
        
        //---------------------------------------------------------------------
        void WrapperCompiler::parseNamespaceContents(NamespacePtr namespaceObj) throw (FailureWithLine)
        {
          while (hasMoreTokens()) {
            if (parseDocumentation()) continue;
            if (parseSemiColon()) continue;
            if (parseDirective()) continue;
            if (parseNamespace(namespaceObj)) continue;
            if (parseUsing(namespaceObj)) continue;
          }
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::parseUsing(NamespacePtr namespaceObj) throw (FailureWithLine)
        {
          auto token = peekNextToken("using");
          if (TokenType_Identifier != token->mTokenType) return false;

          if ("using" != token->mToken) return false;

          extractNextToken("using");  // skip "using"

          token = peekNextToken("using");
          if (TokenType_Identifier == token->mTokenType) {
            if ("namespace" == token->mToken) {
              extractNextToken("using");  // skip "namespace"

              // extract until ";" found
              String namespacePathStr;
              
              token = peekNextToken("using");
              while (TokenType_SemiColon != token->mTokenType) {
                extractNextToken("using"); // skip it
                namespacePathStr += token->mToken;
              }
              
              auto foundNamespace = namespaceObj->findNamespace(namespacePathStr);
              if (!foundNamespace) {
                ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, token->mLineCount, String("using namespace was not found:") + namespacePathStr);
              }

              processUsingNamespace(namespaceObj, foundNamespace);
              return true;
            }
          }

          // extract until ";" found
          String typePathStr;

          token = peekNextToken("using");
          while (TokenType_SemiColon != token->mTokenType) {
            extractNextToken("using"); // skip it
            typePathStr += token->mToken;
          }

          auto foundType = namespaceObj->toContext()->findType(typePathStr);
          if (!foundType) {
            ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, token->mLineCount, String("using type was not found:") + typePathStr);
          }
          
          processUsingType(namespaceObj, foundType);
          return true;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::parseDocumentation()
        {
          bool found = false;

          while (hasMoreTokens()) {
            auto token = peekNextToken("documentation");
            if (TokenType_Documentation != token->mTokenType) return found;

            found = true;
            mPendingDocumentation.push_back(extractNextToken("documentation"));
          }

          return found;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::parseSemiColon()
        {
          auto token = peekNextToken(";");

          if (TokenType_SemiColon != token->mTokenType) return false;
          extractNextToken(";");
          return true;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::parseDirective() throw (FailureWithLine)
        {
          auto token = peekNextToken("directive");

          if (TokenType_Directive != token->mTokenType) return false;
          extractNextToken("directive");

          pushDirectiveTokens(token);

          {
            bool ignoreMode = false;
            do {
              if (!parseDirectiveExclusive(ignoreMode)) goto done;

              if (!ignoreMode) goto done;
              popTokens();

              while (hasMoreTokens()) {
                token = extractNextToken("directive");
                if (pushDirectiveTokens(token)) goto check_exclusive_again;
              }
              break;

            check_exclusive_again:
              {
              }

            } while (ignoreMode);
          }

        done:
          {
          }

          popTokens();

          return true;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::pushDirectiveTokens(TokenPtr token) throw (FailureWithLine)
        {
          if (!token) return false;
          if (TokenType_Directive != token->mTokenType) return false;

          TokenList tokens;
          tokenize(token->mToken.c_str(), tokens, token->mLineCount);

          pushTokens(tokens);
          return false;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::parseDirectiveExclusive(bool &outIgnoreMode) throw (FailureWithLine)
        {
          auto token = peekNextToken("Directive " ZS_WRAPPER_COMPILER_DIRECTIVE_EXCLUSIZE);

          if (TokenType_Identifier != token->mTokenType) return false;
          if (ZS_WRAPPER_COMPILER_DIRECTIVE_EXCLUSIZE != token->mToken) return false;

          outIgnoreMode = true;

          extractNextToken("Directive " ZS_WRAPPER_COMPILER_DIRECTIVE_EXCLUSIZE);

          token = extractNextToken("Directive " ZS_WRAPPER_COMPILER_DIRECTIVE_EXCLUSIZE);
          if (TokenType_Identifier != token->mTokenType) {
            ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, token->mLineCount, String("Macro ZS_WRAPPER_COMPILER_MACRO_EXCLUSIZE expecting identifier"));
          }

          String exclusiveId = token->mToken;

          if ((0 == exclusiveId.compareNoCase("x")) ||
              (mConfig.mProject->mDefinedExclusives.end() != mConfig.mProject->mDefinedExclusives.find(exclusiveId)))
          {
            outIgnoreMode = false;
          }
          return true;
        }

        //---------------------------------------------------------------------
        ElementPtr WrapperCompiler::getDocumentation()
        {
          if (mPendingDocumentation.size() < 1) return ElementPtr();

          String resultStr = "<doc>";
          bool first = true;
          while (mPendingDocumentation.size() > 0) {
            auto token = mPendingDocumentation.front();
            if (!first) {
              resultStr += " ";
            }

            resultStr += token->mToken;
            first = false;

            mPendingDocumentation.pop_front();
          }

          resultStr += "</doc>";
          return UseHelper::toXML(resultStr);
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::mergeDocumentation(ElementPtr &existingDocumentation)
        {
          auto rootEl = getDocumentation();
          if (!rootEl) return;

          if (!existingDocumentation) {
            existingDocumentation = rootEl;
            return;
          }
          
          auto childEl = rootEl->getFirstChild();
          while (childEl) {
            auto nextEl = childEl->getNextSibling();
            childEl->orphan();
            existingDocumentation->adoptAsLastChild(childEl);
            childEl = nextEl;
          }
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::pushTokens(const TokenList &tokens)
        {
          mTokenListStack.push(make_shared<TokenList>(tokens));
          if (tokens.size() > 0) {
            mLastTokenStack.push(tokens.front());
          } else {
            mLastTokenStack.push(TokenPtr());
          }
        }
        //---------------------------------------------------------------------
        void WrapperCompiler::pushTokens(TokenListPtr tokens)
        {
          mTokenListStack.push(tokens);
          if (tokens->size() > 0) {
            mLastTokenStack.push(tokens->front());
          } else {
            mLastTokenStack.push(TokenPtr());
          }
        }

        //---------------------------------------------------------------------
        WrapperCompiler::TokenListPtr WrapperCompiler::getTokens() const
        {
          if (mTokenListStack.size() < 1) return TokenListPtr();
          return mTokenListStack.top();
        }

        //---------------------------------------------------------------------
        WrapperCompiler::TokenListPtr WrapperCompiler::popTokens()
        {
          TokenListPtr result = mTokenListStack.top();

          mTokenListStack.pop();
          mLastTokenStack.pop();

          if (mLastTokenStack.size() > 0) {
            auto token = mLastTokenStack.top();
            if (token) mLastToken = token;
          }

          return result;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::hasMoreTokens() const
        {
          if (mTokenListStack.size() < 1) return false;
          if (getTokens()->size() < 1) return false;
          return true;
        }

        //---------------------------------------------------------------------
        TokenPtr WrapperCompiler::peekNextToken(const char *whatExpectingMoreTokens) throw (FailureWithLine)
        {
          if (mTokenListStack.size() > 0) {
            if (getTokens()->size() > 0) return getTokens()->front();
          }

          TokenPtr lastToken;
          if (mLastTokenStack.size() > 0) {
            mLastTokenStack.top();
          } else {
            lastToken = mLastToken;
          }

          ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_UNEXPECTED_EOF, lastToken ? lastToken->mLineCount : 0, String(whatExpectingMoreTokens) + " unexpectedly reached EOF");
          return TokenPtr();
        }

        //---------------------------------------------------------------------
        TokenPtr WrapperCompiler::extractNextToken(const char *whatExpectingMoreTokens) throw (FailureWithLine)
        {
          if (mTokenListStack.size() > 0) {
            if (getTokens()->size() > 0) {
              mLastToken = getTokens()->front();
              mLastTokenStack.pop();
              mLastTokenStack.push(mLastToken);
              getTokens()->pop_front();
              return mLastToken;
            }
          }

          TokenPtr lastToken;
          if (mLastTokenStack.size() > 0) {
            mLastTokenStack.top();
          } else {
            lastToken = mLastToken;
          }

          ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_UNEXPECTED_EOF, lastToken ? lastToken->mLineCount : 0, String(whatExpectingMoreTokens) + " unexpectedly reached EOF");
          return TokenPtr();
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::putBackToken(TokenPtr token)
        {
          if (mTokenListStack.size() < 1) {
            ZS_THROW_INVALID_USAGE("must have active stack of tokens");
          }

          auto tokens = getTokens();
          tokens->push_front(token);

          mLastToken = token;
          mLastTokenStack.pop();
          mLastTokenStack.push(token);
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::putBackTokens(const TokenList &tokens)
        {
          if (mTokenListStack.size() < 1) {
            ZS_THROW_INVALID_USAGE("must have active stack of tokens");
          }

          auto existingTokens = getTokens();

          TokenPtr lastToken;
          for (auto riter = tokens.rbegin(); riter != tokens.rend(); ++riter)
          {
            lastToken = (*riter);
            existingTokens->push_front(lastToken);
          }

          mLastToken = lastToken;
          mLastTokenStack.pop();
          mLastTokenStack.push(lastToken);
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::extractToClosingBraceToken(
                                                         const char *whatExpectingClosingToken,
                                                         TokenList &outTokens,
                                                         bool includeOuterBrace
                                                         ) throw (FailureWithLine)
        {
          auto token = peekNextToken(whatExpectingClosingToken);
          if (!token->isBrace()) return false;
          if (!token->isOpenBrace()) return false;


          size_t countBrace = 0;
          size_t countCurly = 0;
          size_t countSquare = 0;
          size_t countAngle = 0;

          do {
            token = extractNextToken(whatExpectingClosingToken);
            outTokens.push_back(token);

            if (token->isBrace()) {
              if (token->isOpenBrace()) {
                switch (token->mTokenType) {
                  case TokenType_Brace:       ++countBrace; break;
                  case TokenType_CurlyBrace:  ++countCurly; break;
                  case TokenType_SquareBrace: ++countSquare; break;
                  case TokenType_AngleBrace:  ++countAngle; break;
                  default:                    break;
                }
              } else {
                switch (token->mTokenType) {
                  case TokenType_Brace:       if (countBrace < 1) goto brace_mismatch; --countBrace; break;
                  case TokenType_CurlyBrace:  if (countCurly < 1) goto brace_mismatch; --countCurly; break;
                  case TokenType_SquareBrace: if (countSquare < 1) goto brace_mismatch; --countSquare; break;
                  case TokenType_AngleBrace:  if (countAngle < 1) goto brace_mismatch; --countAngle; break;
                  default:                    break;
                }
              }
            }
          } while ((countBrace > 0) ||
                   (countCurly > 0) ||
                   (countSquare > 0) ||
                   (countAngle > 0));

          {
            goto done;
          }

        brace_mismatch:
          {
            ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, mLastToken ? mLastToken->mLineCount : 0, String(whatExpectingClosingToken) + " brace mismatch");
          }

        done:
          {
            if (!includeOuterBrace) {
              if (outTokens.size() > 1) {
                outTokens.pop_front();
                outTokens.pop_back();
              }
            }
          }

          return true;
        }

        //---------------------------------------------------------------------
        bool WrapperCompiler::extractToComma(
                                             const char *whatExpectingComma,
                                             TokenList &outTokens
                                             ) throw (FailureWithLine)
        {
          while (hasMoreTokens()) {
            auto token = extractNextToken(whatExpectingComma);
            if (TokenType_CommaOperator == token->mTokenType) break;

            if (token->isBrace()) {
              putBackToken(token);
              if (token->isCloseBrace()) return true;

              TokenList braceTokens;
              extractToClosingBraceToken(whatExpectingComma, braceTokens, true);
              for (auto iter = braceTokens.begin(); iter != braceTokens.end(); ++iter) {
                outTokens.push_back(*iter);
              }
              continue;
            }
            outTokens.push_back(token);
          }
          return true;
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::processUsingNamespace(
                                                    NamespacePtr currentNamespace,
                                                    NamespacePtr usingNamespace
                                                    )
        {
          if (currentNamespace == usingNamespace) return;

          for (auto iter = usingNamespace->mEnums.begin(); iter != usingNamespace->mEnums.end(); ++iter)
          {
            auto name = (*iter).first;
            auto type = (*iter).second;
            
            auto found = currentNamespace->mTypedefs.find(name);
            if (found != currentNamespace->mTypedefs.end()) continue;
            
            auto newTypedef = TypedefType::create(currentNamespace);
            newTypedef->mName = name;
            newTypedef->mOriginalType = type;
            currentNamespace->mTypedefs[name] = newTypedef;
          }

          for (auto iter = usingNamespace->mStructs.begin(); iter != usingNamespace->mStructs.end(); ++iter)
          {
            auto name = (*iter).first;
            auto type = (*iter).second;
            
            auto found = currentNamespace->mTypedefs.find(name);
            if (found != currentNamespace->mTypedefs.end()) continue;

            auto newTypedef = TypedefType::create(currentNamespace);
            newTypedef->mName = name;
            newTypedef->mOriginalType = type;
            currentNamespace->mTypedefs[name] = newTypedef;
          }

          for (auto iter = usingNamespace->mTypedefs.begin(); iter != usingNamespace->mTypedefs.end(); ++iter)
          {
            auto name = (*iter).first;
            auto type = (*iter).second;

            auto found = currentNamespace->mTypedefs.find(name);
            if (found != currentNamespace->mTypedefs.end()) continue;

            auto newTypedef = TypedefType::create(currentNamespace);
            newTypedef->mName = name;
            newTypedef->mOriginalType = type;
            currentNamespace->mTypedefs[name] = newTypedef;
          }
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::processUsingType(
                                               NamespacePtr currentNamespace,
                                               TypePtr usingType
                                               )
        {
          auto name = usingType->getMappingName();
          
          auto found = currentNamespace->mTypedefs.find(name);
          if (found != currentNamespace->mTypedefs.end()) return;

          auto newTypedef = TypedefType::create(currentNamespace);
          newTypedef->mName = name;
          newTypedef->mOriginalType = usingType;
          currentNamespace->mTypedefs[name] = newTypedef;
        }

        //---------------------------------------------------------------------
        WrapperCompiler::TypePtr WrapperCompiler::findTypeOrCreateTypedef(
                                                                          ContextPtr context,
                                                                          const TokenList &inTokens,
                                                                          TypedefPtr &outCreatedTypedef
                                                                          ) throw (FailureWithLine)
        {
          const char *what = "Type search";

          TokenList pretemplateTokens;

          {
            // search for template parameters
            pushTokens(inTokens);

            TypeList templateTypes;

            while (hasMoreTokens()) {
              auto token = extractNextToken(what);
              pretemplateTokens.push_back(token);

              if (TokenType_AngleBrace == token->mTokenType) {
                putBackToken(token);
                TokenList templateContents;
                extractToClosingBraceToken(what, templateContents);

                pushTokens(templateContents);

                while (hasMoreTokens()) {
                  TokenList templateTypeTokens;
                  extractToComma(what, templateTypeTokens);

                  TypedefPtr typedefObj;
                  auto foundType = findTypeOrCreateTypedef(context, templateTypeTokens, typedefObj);
                  templateTypes.push_back(foundType);
                }

                popTokens();
                break;
              }
            }

            popTokens();
          }

          {
            pushTokens(pretemplateTokens);

            bool foundAnyBasicTypeModifiers = false;
            bool foundAnyOtherModifier = false;

            bool foundSigned = false;
            bool foundUnsigned = false;
            bool foundChar = false;
            bool foundShort = false;
            bool foundInt = false;
            size_t totalLongsFound = false;
            bool foundFloat = false;
            bool foundDouble = false;

            bool foundConst = false;
            bool foundPointer = false;
            bool foundAddress = false;
            bool foundCarot = false;

            String typeName;

            while (hasMoreTokens()) {
              auto token = extractNextToken(what);
              switch (token->mTokenType) {
                case TokenType_Identifier: {
                  if ("signed" == token->mToken) {
                    if (foundUnsigned || foundSigned || foundFloat || foundDouble) goto found_incompatible_modifier;
                    foundSigned = true;
                    goto found_basic_type_modifier;
                  }
                  if ("unsigned" == token->mToken) {
                    if (foundUnsigned || foundSigned || foundFloat || foundDouble) goto found_incompatible_modifier;
                    foundUnsigned = true;
                    goto found_basic_type_modifier;
                  }
                  if ("long" == token->mToken) {
                    if ((totalLongsFound > 1) || foundChar || foundShort || foundFloat) goto found_incompatible_modifier;
                    if ((totalLongsFound > 1) && (foundDouble)) goto found_incompatible_modifier;
                    ++totalLongsFound;
                    goto found_basic_type_modifier;
                  }
                  if ("char" == token->mToken) {
                    if ((totalLongsFound > 0) || foundChar || foundShort || foundInt || foundFloat || foundDouble) goto found_incompatible_modifier;
                    foundChar = true;
                    goto found_basic_type_modifier;
                  }
                  if ("short" == token->mToken) {
                    if ((totalLongsFound > 0) || foundChar || foundShort || foundFloat || foundDouble) goto found_incompatible_modifier;
                    foundShort = true;
                    goto found_basic_type_modifier;
                  }
                  if ("int" == token->mToken) {
                    if (foundChar || foundInt || foundFloat || foundDouble) goto found_incompatible_modifier;
                    foundInt = true;
                    goto found_basic_type_modifier;
                  }
                  if ("long" == token->mToken) {
                    if ((totalLongsFound > 1) || foundChar || foundShort || foundFloat) goto found_incompatible_modifier;
                    if ((totalLongsFound > 0) && foundDouble) goto found_incompatible_modifier;
                    ++totalLongsFound;
                    goto found_basic_type_modifier;
                  }
                  if ("float" == token->mToken) {
                    if (foundSigned || foundUnsigned || (totalLongsFound > 0) || foundChar || foundInt || foundFloat || foundDouble) goto found_incompatible_modifier;
                    foundFloat = true;
                    goto found_basic_type_modifier;
                  }
                  if ("double" == token->mToken) {
                    if (foundSigned || foundUnsigned || (totalLongsFound > 1) || foundChar || foundInt || foundFloat || foundDouble) goto found_incompatible_modifier;
                    foundDouble = true;
                    goto found_basic_type_modifier;
                  }
                  if ("const" == token->mToken) {
                    if (foundConst) goto found_incompatible_modifier;
                    foundConst = true;
                    goto found_other_modifier;
                  }

                  if (typeName.hasData()) {
                    ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, mLastToken ? mLastToken->mLineCount : 0, String(what) + " has type name redeclared");
                  }
                  typeName = token->mToken;
                  goto found_name;
                }
                case TokenType_PointerOperator: {
                  if (foundCarot || foundPointer) goto found_incompatible_modifier; // carot or double pointer not supported, use address operator by reference
                  foundPointer = true;
                  break;
                }
                case TokenType_AddressOperator: {
                  if (foundAddress) goto found_incompatible_modifier;
                  foundPointer = true;
                  break;
                }
                case TokenType_CarotOperator: {
                  if (foundAnyBasicTypeModifiers || foundAddress) goto found_incompatible_modifier;
                  foundCarot = true;
                  break;
                }
                default:
                {
                  ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, mLastToken ? mLastToken->mLineCount : 0, String(what) + " has not legal type modifier");
                  break;
                }
              }

            found_other_modifier:
              {
                foundAnyOtherModifier = true;
                continue;
              }

            found_basic_type_modifier:
              {
                if (foundCarot) goto found_incompatible_modifier;
                foundAnyBasicTypeModifiers = true;
                continue;
              }

            found_name:
              {
                continue;
              }
            }

            goto process_type;

#define TODO 1
#define TODO 2

          process_type:
            {
              if ((foundShort) && (foundInt)) foundInt = false; // strip redundant information

              IEventingTypes::PredefinedTypedefs predefinedType {IEventingTypes::PredefinedTypedef_First};

              // scope: check type information
              {
                if (typeName.hasData()) {
                  if (foundAnyBasicTypeModifiers) goto found_incompatible_modifier;

                  try {
                    auto foundBasicType = IEventingTypes::toPredefinedTypedef(typeName);
                    switch (foundBasicType) {
                      case IEventingTypes::PredefinedTypedef_pointer:

                      case IEventingTypes::PredefinedTypedef_binary:
                      case IEventingTypes::PredefinedTypedef_size:

                      case IEventingTypes::PredefinedTypedef_string:
                      case IEventingTypes::PredefinedTypedef_astring:
                      case IEventingTypes::PredefinedTypedef_wstring:   goto found_incompatible_modifier;
                    }

                    predefinedType = foundBasicType;
                    goto found_basic_type;
                  } catch (const InvalidArgument &) {
                    // not a basic type
                  }
                }

                if (foundAnyBasicTypeModifiers) {
                }
              }

            found_basic_type:
              {
              }

              popTokens();
            }
            
          found_incompatible_modifier:
            {
              ZS_THROW_CUSTOM_PROPERTIES_2(FailureWithLine, ZS_EVENTING_TOOL_INVALID_CONTENT, mLastToken ? mLastToken->mLineCount : 0, String(what) + " has invalid type modifier");
            }

          }
          return TypePtr();
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::writeXML(const String &outputName, const DocumentPtr &doc) const throw (Failure)
        {
          if (!doc) return;
          try {
            auto output = UseHelper::writeXML(*doc);
            UseHelper::saveFile(outputName, *output);
          } catch (const StdError &e) {
            ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_SYSTEM_ERROR, "Failed to save XML file \"" + outputName + "\": " + " error=" + string(e.result()) + ", reason=" + e.message());
          }
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::writeJSON(const String &outputName, const DocumentPtr &doc) const throw (Failure)
        {
          if (!doc) return;
          try {
            auto output = UseHelper::writeJSON(*doc);
            UseHelper::saveFile(outputName, *output);
          } catch (const StdError &e) {
            ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_SYSTEM_ERROR, "Failed to save JSON file \"" + outputName + "\": " + " error=" + string(e.result()) + ", reason=" + e.message());
          }
        }

        //---------------------------------------------------------------------
        void WrapperCompiler::writeBinary(const String &outputName, const SecureByteBlockPtr &buffer) const throw (Failure)
        {
          if ((!buffer) ||
              (0 == buffer->SizeInBytes())) {
            ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_SYSTEM_ERROR, "Failed to save file \"" + outputName + "\": file is empty");
          }
          try {
            UseHelper::saveFile(outputName, *buffer);
          } catch (const StdError &e) {
            ZS_THROW_CUSTOM_PROPERTIES_1(Failure, ZS_EVENTING_TOOL_SYSTEM_ERROR, "Failed to save file \"" + outputName + "\": " + " error=" + string(e.result()) + ", reason=" + e.message());
          }
        }

      } // namespace internal
    } // namespace tool
  } // namespace eventing
} // namespace zsLib
