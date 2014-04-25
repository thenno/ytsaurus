// A Bison parser, made by GNU Bison 3.0.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2013 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.

// Take the name prefix into account.
#define yylex   yt_ql_yylex

// First part of user declarations.


# ifndef YY_NULL
#  if defined __cplusplus && 201103L <= __cplusplus
#   define YY_NULL nullptr
#  else
#   define YY_NULL 0
#  endif
# endif

#include "parser.hpp"

// User implementation prologue.

// Unqualified %code blocks.

    #include <ytlib/query_client/lexer.h>
    #define yt_ql_yylex lexer.GetNextToken

    #ifndef YYLLOC_DEFAULT
    #define YYLLOC_DEFAULT(Current, Rhs, N) \
        do { \
            if (N) { \
                (Current).first = YYRHSLOC(Rhs, 1).first; \
                (Current).second = YYRHSLOC (Rhs, N).second; \
            } else { \
                (Current).first = (Current).second = YYRHSLOC(Rhs, 0).second; \
            } \
        } while (false)
    #endif



#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> // FIXME: INFRINGES ON USER NAME SPACE.
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

# ifndef YYLLOC_DEFAULT
#  define YYLLOC_DEFAULT(Current, Rhs, N)                               \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).begin  = YYRHSLOC (Rhs, 1).begin;                   \
          (Current).end    = YYRHSLOC (Rhs, N).end;                     \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).begin = (Current).end = YYRHSLOC (Rhs, 0).end;      \
        }                                                               \
    while (/*CONSTCOND*/ false)
# endif


// Suppress unused-variable warnings by "using" E.
#define YYUSE(E) ((void) (E))

// Enable debugging if requested.
#if YT_QL_YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
# define YYCDEBUG if (yydebug_) (*yycdebug_)

# define YY_SYMBOL_PRINT(Title, Symbol)         \
  do {                                          \
    if (yydebug_)                               \
    {                                           \
      *yycdebug_ << Title << ' ';               \
      yy_print_ (*yycdebug_, Symbol);           \
      *yycdebug_ << std::endl;                  \
    }                                           \
  } while (false)

# define YY_REDUCE_PRINT(Rule)          \
  do {                                  \
    if (yydebug_)                       \
      yy_reduce_print_ (Rule);          \
  } while (false)

# define YY_STACK_PRINT()               \
  do {                                  \
    if (yydebug_)                       \
      yystack_print_ ();                \
  } while (false)

#else // !YT_QL_YYDEBUG

# define YYCDEBUG if (false) std::cerr
# define YY_SYMBOL_PRINT(Title, Symbol)  YYUSE(Symbol)
# define YY_REDUCE_PRINT(Rule)           static_cast<void>(0)
# define YY_STACK_PRINT()                static_cast<void>(0)

#endif // !YT_QL_YYDEBUG

#define yyerrok         (yyerrstatus_ = 0)
#define yyclearin       (yyempty = true)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYRECOVERING()  (!!yyerrstatus_)

namespace NYT { namespace NQueryClient {

  /* Return YYSTR after stripping away unnecessary quotes and
     backslashes, so that it's suitable for yyerror.  The heuristic is
     that double-quoting is unnecessary unless the string contains an
     apostrophe, a comma, or backslash (other than backslash-backslash).
     YYSTR is taken from yytname.  */
  std::string
  TParser::yytnamerr_ (const char *yystr)
  {
    if (*yystr == '"')
      {
        std::string yyr = "";
        char const *yyp = yystr;

        for (;;)
          switch (*++yyp)
            {
            case '\'':
            case ',':
              goto do_not_strip_quotes;

            case '\\':
              if (*++yyp != '\\')
                goto do_not_strip_quotes;
              // Fall through.
            default:
              yyr += *yyp;
              break;

            case '"':
              return yyr;
            }
      do_not_strip_quotes: ;
      }

    return yystr;
  }


  /// Build a parser object.
  TParser::TParser (TLexer& lexer_yyarg, TPlanContext* context_yyarg, const TOperator** head_yyarg)
    :
#if YT_QL_YYDEBUG
      yydebug_ (false),
      yycdebug_ (&std::cerr),
#endif
      lexer (lexer_yyarg),
      context (context_yyarg),
      head (head_yyarg)
  {}

  TParser::~TParser ()
  {}


  /*---------------.
  | Symbol types.  |
  `---------------*/

  inline
  TParser::syntax_error::syntax_error (const location_type& l, const std::string& m)
    : std::runtime_error (m)
    , location (l)
  {}

  // basic_symbol.
  template <typename Base>
  inline
  TParser::basic_symbol<Base>::basic_symbol ()
    : value ()
  {}

  template <typename Base>
  inline
  TParser::basic_symbol<Base>::basic_symbol (const basic_symbol& other)
    : Base (other)
    , value ()
    , location (other.location)
  {
      switch (other.type_get ())
    {
      case 44: // relational-op
      case 46: // additive-op
      case 48: // multiplicative-op
        value.copy< EBinaryOp > (other.value);
        break;

      case 40: // expression
      case 41: // or-op-expr
      case 42: // and-op-expr
      case 43: // relational-op-expr
      case 45: // additive-op-expr
      case 47: // multiplicative-op-expr
      case 49: // atomic-expr
        value.copy< TExpression* > (other.value);
        break;

      case 36: // where-clause
        value.copy< TFilterOperator* > (other.value);
        break;

      case 51: // function-expr
        value.copy< TFunctionExpression* > (other.value);
        break;

      case 52: // function-expr-args
        value.copy< TFunctionExpression::TArguments > (other.value);
        break;

      case 37: // group-by-clause
        value.copy< TGroupOperator* > (other.value);
        break;

      case 39: // named-expression
        value.copy< TNamedExpression > (other.value);
        break;

      case 38: // named-expression-list
        value.copy< TNamedExpressionList > (other.value);
        break;

      case 33: // head-clause
        value.copy< TOperator* > (other.value);
        break;

      case 34: // select-clause
        value.copy< TProjectOperator* > (other.value);
        break;

      case 50: // reference-expr
        value.copy< TReferenceExpression* > (other.value);
        break;

      case 35: // from-clause
        value.copy< TScanOperator* > (other.value);
        break;

      case 13: // "identifier"
      case 16: // "YPath literal"
        value.copy< TStringBuf > (other.value);
        break;

      case 15: // "double literal"
        value.copy< double > (other.value);
        break;

      case 14: // "integer literal"
        value.copy< i64 > (other.value);
        break;

      default:
        break;
    }

  }


  template <typename Base>
  inline
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const semantic_type& v, const location_type& l)
    : Base (t)
    , value ()
    , location (l)
  {
    (void) v;
      switch (this->type_get ())
    {
      case 44: // relational-op
      case 46: // additive-op
      case 48: // multiplicative-op
        value.copy< EBinaryOp > (v);
        break;

      case 40: // expression
      case 41: // or-op-expr
      case 42: // and-op-expr
      case 43: // relational-op-expr
      case 45: // additive-op-expr
      case 47: // multiplicative-op-expr
      case 49: // atomic-expr
        value.copy< TExpression* > (v);
        break;

      case 36: // where-clause
        value.copy< TFilterOperator* > (v);
        break;

      case 51: // function-expr
        value.copy< TFunctionExpression* > (v);
        break;

      case 52: // function-expr-args
        value.copy< TFunctionExpression::TArguments > (v);
        break;

      case 37: // group-by-clause
        value.copy< TGroupOperator* > (v);
        break;

      case 39: // named-expression
        value.copy< TNamedExpression > (v);
        break;

      case 38: // named-expression-list
        value.copy< TNamedExpressionList > (v);
        break;

      case 33: // head-clause
        value.copy< TOperator* > (v);
        break;

      case 34: // select-clause
        value.copy< TProjectOperator* > (v);
        break;

      case 50: // reference-expr
        value.copy< TReferenceExpression* > (v);
        break;

      case 35: // from-clause
        value.copy< TScanOperator* > (v);
        break;

      case 13: // "identifier"
      case 16: // "YPath literal"
        value.copy< TStringBuf > (v);
        break;

      case 15: // "double literal"
        value.copy< double > (v);
        break;

      case 14: // "integer literal"
        value.copy< i64 > (v);
        break;

      default:
        break;
    }
}


  // Implementation of basic_symbol constructor for each type.

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const location_type& l)
    : Base (t)
    , value ()
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const EBinaryOp v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TExpression* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TFilterOperator* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TFunctionExpression* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TFunctionExpression::TArguments v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TGroupOperator* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TNamedExpression v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TNamedExpressionList v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TOperator* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TProjectOperator* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TReferenceExpression* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TScanOperator* v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TStringBuf v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const double v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const i64 v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}


  template <typename Base>
  inline
  TParser::basic_symbol<Base>::~basic_symbol ()
  {
    // User destructor.
    symbol_number_type yytype = this->type_get ();
    switch (yytype)
    {
   default:
      break;
    }

    // Type destructor.
    switch (yytype)
    {
      case 44: // relational-op
      case 46: // additive-op
      case 48: // multiplicative-op
        value.template destroy< EBinaryOp > ();
        break;

      case 40: // expression
      case 41: // or-op-expr
      case 42: // and-op-expr
      case 43: // relational-op-expr
      case 45: // additive-op-expr
      case 47: // multiplicative-op-expr
      case 49: // atomic-expr
        value.template destroy< TExpression* > ();
        break;

      case 36: // where-clause
        value.template destroy< TFilterOperator* > ();
        break;

      case 51: // function-expr
        value.template destroy< TFunctionExpression* > ();
        break;

      case 52: // function-expr-args
        value.template destroy< TFunctionExpression::TArguments > ();
        break;

      case 37: // group-by-clause
        value.template destroy< TGroupOperator* > ();
        break;

      case 39: // named-expression
        value.template destroy< TNamedExpression > ();
        break;

      case 38: // named-expression-list
        value.template destroy< TNamedExpressionList > ();
        break;

      case 33: // head-clause
        value.template destroy< TOperator* > ();
        break;

      case 34: // select-clause
        value.template destroy< TProjectOperator* > ();
        break;

      case 50: // reference-expr
        value.template destroy< TReferenceExpression* > ();
        break;

      case 35: // from-clause
        value.template destroy< TScanOperator* > ();
        break;

      case 13: // "identifier"
      case 16: // "YPath literal"
        value.template destroy< TStringBuf > ();
        break;

      case 15: // "double literal"
        value.template destroy< double > ();
        break;

      case 14: // "integer literal"
        value.template destroy< i64 > ();
        break;

      default:
        break;
    }

  }

  template <typename Base>
  inline
  void
  TParser::basic_symbol<Base>::move (basic_symbol& s)
  {
    super_type::move(s);
      switch (this->type_get ())
    {
      case 44: // relational-op
      case 46: // additive-op
      case 48: // multiplicative-op
        value.move< EBinaryOp > (s.value);
        break;

      case 40: // expression
      case 41: // or-op-expr
      case 42: // and-op-expr
      case 43: // relational-op-expr
      case 45: // additive-op-expr
      case 47: // multiplicative-op-expr
      case 49: // atomic-expr
        value.move< TExpression* > (s.value);
        break;

      case 36: // where-clause
        value.move< TFilterOperator* > (s.value);
        break;

      case 51: // function-expr
        value.move< TFunctionExpression* > (s.value);
        break;

      case 52: // function-expr-args
        value.move< TFunctionExpression::TArguments > (s.value);
        break;

      case 37: // group-by-clause
        value.move< TGroupOperator* > (s.value);
        break;

      case 39: // named-expression
        value.move< TNamedExpression > (s.value);
        break;

      case 38: // named-expression-list
        value.move< TNamedExpressionList > (s.value);
        break;

      case 33: // head-clause
        value.move< TOperator* > (s.value);
        break;

      case 34: // select-clause
        value.move< TProjectOperator* > (s.value);
        break;

      case 50: // reference-expr
        value.move< TReferenceExpression* > (s.value);
        break;

      case 35: // from-clause
        value.move< TScanOperator* > (s.value);
        break;

      case 13: // "identifier"
      case 16: // "YPath literal"
        value.move< TStringBuf > (s.value);
        break;

      case 15: // "double literal"
        value.move< double > (s.value);
        break;

      case 14: // "integer literal"
        value.move< i64 > (s.value);
        break;

      default:
        break;
    }

    location = s.location;
  }

  // by_type.
  inline
  TParser::by_type::by_type ()
     : type (empty)
  {}

  inline
  TParser::by_type::by_type (const by_type& other)
    : type (other.type)
  {}

  inline
  TParser::by_type::by_type (token_type t)
    : type (yytranslate_ (t))
  {}

  inline
  void
  TParser::by_type::move (by_type& that)
  {
    type = that.type;
    that.type = empty;
  }

  inline
  int
  TParser::by_type::type_get () const
  {
    return type;
  }
  // Implementation of make_symbol for each symbol type.
  TParser::symbol_type
  TParser::make_End (const location_type& l)
  {
    return symbol_type (token::End, l);

  }

  TParser::symbol_type
  TParser::make_Failure (const location_type& l)
  {
    return symbol_type (token::Failure, l);

  }

  TParser::symbol_type
  TParser::make_StrayWillParseQuery (const location_type& l)
  {
    return symbol_type (token::StrayWillParseQuery, l);

  }

  TParser::symbol_type
  TParser::make_KwFrom (const location_type& l)
  {
    return symbol_type (token::KwFrom, l);

  }

  TParser::symbol_type
  TParser::make_KwWhere (const location_type& l)
  {
    return symbol_type (token::KwWhere, l);

  }

  TParser::symbol_type
  TParser::make_KwGroupBy (const location_type& l)
  {
    return symbol_type (token::KwGroupBy, l);

  }

  TParser::symbol_type
  TParser::make_KwAs (const location_type& l)
  {
    return symbol_type (token::KwAs, l);

  }

  TParser::symbol_type
  TParser::make_KwAnd (const location_type& l)
  {
    return symbol_type (token::KwAnd, l);

  }

  TParser::symbol_type
  TParser::make_KwOr (const location_type& l)
  {
    return symbol_type (token::KwOr, l);

  }

  TParser::symbol_type
  TParser::make_KwBetween (const location_type& l)
  {
    return symbol_type (token::KwBetween, l);

  }

  TParser::symbol_type
  TParser::make_KwIn (const location_type& l)
  {
    return symbol_type (token::KwIn, l);

  }

  TParser::symbol_type
  TParser::make_Identifier (const TStringBuf& v, const location_type& l)
  {
    return symbol_type (token::Identifier, v, l);

  }

  TParser::symbol_type
  TParser::make_IntegerLiteral (const i64& v, const location_type& l)
  {
    return symbol_type (token::IntegerLiteral, v, l);

  }

  TParser::symbol_type
  TParser::make_DoubleLiteral (const double& v, const location_type& l)
  {
    return symbol_type (token::DoubleLiteral, v, l);

  }

  TParser::symbol_type
  TParser::make_YPathLiteral (const TStringBuf& v, const location_type& l)
  {
    return symbol_type (token::YPathLiteral, v, l);

  }

  TParser::symbol_type
  TParser::make_OpModulo (const location_type& l)
  {
    return symbol_type (token::OpModulo, l);

  }

  TParser::symbol_type
  TParser::make_LeftParenthesis (const location_type& l)
  {
    return symbol_type (token::LeftParenthesis, l);

  }

  TParser::symbol_type
  TParser::make_RightParenthesis (const location_type& l)
  {
    return symbol_type (token::RightParenthesis, l);

  }

  TParser::symbol_type
  TParser::make_Asterisk (const location_type& l)
  {
    return symbol_type (token::Asterisk, l);

  }

  TParser::symbol_type
  TParser::make_OpPlus (const location_type& l)
  {
    return symbol_type (token::OpPlus, l);

  }

  TParser::symbol_type
  TParser::make_Comma (const location_type& l)
  {
    return symbol_type (token::Comma, l);

  }

  TParser::symbol_type
  TParser::make_OpMinus (const location_type& l)
  {
    return symbol_type (token::OpMinus, l);

  }

  TParser::symbol_type
  TParser::make_OpDivide (const location_type& l)
  {
    return symbol_type (token::OpDivide, l);

  }

  TParser::symbol_type
  TParser::make_OpLess (const location_type& l)
  {
    return symbol_type (token::OpLess, l);

  }

  TParser::symbol_type
  TParser::make_OpLessOrEqual (const location_type& l)
  {
    return symbol_type (token::OpLessOrEqual, l);

  }

  TParser::symbol_type
  TParser::make_OpEqual (const location_type& l)
  {
    return symbol_type (token::OpEqual, l);

  }

  TParser::symbol_type
  TParser::make_OpNotEqual (const location_type& l)
  {
    return symbol_type (token::OpNotEqual, l);

  }

  TParser::symbol_type
  TParser::make_OpGreater (const location_type& l)
  {
    return symbol_type (token::OpGreater, l);

  }

  TParser::symbol_type
  TParser::make_OpGreaterOrEqual (const location_type& l)
  {
    return symbol_type (token::OpGreaterOrEqual, l);

  }



  // by_state.
  inline
  TParser::by_state::by_state ()
    : state (empty)
  {}

  inline
  TParser::by_state::by_state (const by_state& other)
    : state (other.state)
  {}

  inline
  void
  TParser::by_state::move (by_state& that)
  {
    state = that.state;
    that.state = empty;
  }

  inline
  TParser::by_state::by_state (state_type s)
    : state (s)
  {}

  inline
  TParser::symbol_number_type
  TParser::by_state::type_get () const
  {
    return state == empty ? 0 : yystos_[state];
  }

  inline
  TParser::stack_symbol_type::stack_symbol_type ()
  {}


  inline
  TParser::stack_symbol_type::stack_symbol_type (state_type s, symbol_type& that)
    : super_type (s, that.location)
  {
      switch (that.type_get ())
    {
      case 44: // relational-op
      case 46: // additive-op
      case 48: // multiplicative-op
        value.move< EBinaryOp > (that.value);
        break;

      case 40: // expression
      case 41: // or-op-expr
      case 42: // and-op-expr
      case 43: // relational-op-expr
      case 45: // additive-op-expr
      case 47: // multiplicative-op-expr
      case 49: // atomic-expr
        value.move< TExpression* > (that.value);
        break;

      case 36: // where-clause
        value.move< TFilterOperator* > (that.value);
        break;

      case 51: // function-expr
        value.move< TFunctionExpression* > (that.value);
        break;

      case 52: // function-expr-args
        value.move< TFunctionExpression::TArguments > (that.value);
        break;

      case 37: // group-by-clause
        value.move< TGroupOperator* > (that.value);
        break;

      case 39: // named-expression
        value.move< TNamedExpression > (that.value);
        break;

      case 38: // named-expression-list
        value.move< TNamedExpressionList > (that.value);
        break;

      case 33: // head-clause
        value.move< TOperator* > (that.value);
        break;

      case 34: // select-clause
        value.move< TProjectOperator* > (that.value);
        break;

      case 50: // reference-expr
        value.move< TReferenceExpression* > (that.value);
        break;

      case 35: // from-clause
        value.move< TScanOperator* > (that.value);
        break;

      case 13: // "identifier"
      case 16: // "YPath literal"
        value.move< TStringBuf > (that.value);
        break;

      case 15: // "double literal"
        value.move< double > (that.value);
        break;

      case 14: // "integer literal"
        value.move< i64 > (that.value);
        break;

      default:
        break;
    }

    // that is emptied.
    that.type = empty;
  }

  inline
  TParser::stack_symbol_type&
  TParser::stack_symbol_type::operator= (const stack_symbol_type& that)
  {
    state = that.state;
      switch (that.type_get ())
    {
      case 44: // relational-op
      case 46: // additive-op
      case 48: // multiplicative-op
        value.copy< EBinaryOp > (that.value);
        break;

      case 40: // expression
      case 41: // or-op-expr
      case 42: // and-op-expr
      case 43: // relational-op-expr
      case 45: // additive-op-expr
      case 47: // multiplicative-op-expr
      case 49: // atomic-expr
        value.copy< TExpression* > (that.value);
        break;

      case 36: // where-clause
        value.copy< TFilterOperator* > (that.value);
        break;

      case 51: // function-expr
        value.copy< TFunctionExpression* > (that.value);
        break;

      case 52: // function-expr-args
        value.copy< TFunctionExpression::TArguments > (that.value);
        break;

      case 37: // group-by-clause
        value.copy< TGroupOperator* > (that.value);
        break;

      case 39: // named-expression
        value.copy< TNamedExpression > (that.value);
        break;

      case 38: // named-expression-list
        value.copy< TNamedExpressionList > (that.value);
        break;

      case 33: // head-clause
        value.copy< TOperator* > (that.value);
        break;

      case 34: // select-clause
        value.copy< TProjectOperator* > (that.value);
        break;

      case 50: // reference-expr
        value.copy< TReferenceExpression* > (that.value);
        break;

      case 35: // from-clause
        value.copy< TScanOperator* > (that.value);
        break;

      case 13: // "identifier"
      case 16: // "YPath literal"
        value.copy< TStringBuf > (that.value);
        break;

      case 15: // "double literal"
        value.copy< double > (that.value);
        break;

      case 14: // "integer literal"
        value.copy< i64 > (that.value);
        break;

      default:
        break;
    }

    location = that.location;
    return *this;
  }


  template <typename Base>
  inline
  void
  TParser::yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const
  {
    if (yymsg)
      YY_SYMBOL_PRINT (yymsg, yysym);
  }

#if YT_QL_YYDEBUG
  template <typename Base>
  void
  TParser::yy_print_ (std::ostream& yyo,
                                     const basic_symbol<Base>& yysym) const
  {
    std::ostream& yyoutput = yyo;
    YYUSE (yyoutput);
    symbol_number_type yytype = yysym.type_get ();
    yyo << (yytype < yyntokens_ ? "token" : "nterm")
        << ' ' << yytname_[yytype] << " ("
        << yysym.location << ": ";
    YYUSE (yytype);
    yyo << ')';
  }
#endif

  inline
  void
  TParser::yypush_ (const char* m, state_type s, symbol_type& sym)
  {
    stack_symbol_type t (s, sym);
    yypush_ (m, t);
  }

  inline
  void
  TParser::yypush_ (const char* m, stack_symbol_type& s)
  {
    if (m)
      YY_SYMBOL_PRINT (m, s);
    yystack_.push (s);
  }

  inline
  void
  TParser::yypop_ (unsigned int n)
  {
    yystack_.pop (n);
  }

#if YT_QL_YYDEBUG
  std::ostream&
  TParser::debug_stream () const
  {
    return *yycdebug_;
  }

  void
  TParser::set_debug_stream (std::ostream& o)
  {
    yycdebug_ = &o;
  }


  TParser::debug_level_type
  TParser::debug_level () const
  {
    return yydebug_;
  }

  void
  TParser::set_debug_level (debug_level_type l)
  {
    yydebug_ = l;
  }
#endif // YT_QL_YYDEBUG

  inline TParser::state_type
  TParser::yy_lr_goto_state_ (state_type yystate, int yylhs)
  {
    int yyr = yypgoto_[yylhs - yyntokens_] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
      return yytable_[yyr];
    else
      return yydefgoto_[yylhs - yyntokens_];
  }

  inline bool
  TParser::yy_pact_value_is_default_ (int yyvalue)
  {
    return yyvalue == yypact_ninf_;
  }

  inline bool
  TParser::yy_table_value_is_error_ (int yyvalue)
  {
    return yyvalue == yytable_ninf_;
  }

  int
  TParser::parse ()
  {
    /// Whether yyla contains a lookahead.
    bool yyempty = true;

    // State.
    int yyn;
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The locations where the error started and ended.
    stack_symbol_type yyerror_range[3];

    /// $$ and @$.
    stack_symbol_type yylhs;

    /// The return value of parse ().
    int yyresult;

    // FIXME: This shoud be completely indented.  It is not yet to
    // avoid gratuitous conflicts when merging into the master branch.
    try
      {
    YYCDEBUG << "Starting parse" << std::endl;


    /* Initialize the stack.  The initial state will be set in
       yynewstate, since the latter expects the semantical and the
       location values to have been already stored, initialize these
       stacks with a primary value.  */
    yystack_.clear ();
    yypush_ (YY_NULL, 0, yyla);

    // A new symbol was pushed on the stack.
  yynewstate:
    YYCDEBUG << "Entering state " << yystack_[0].state << std::endl;

    // Accept?
    if (yystack_[0].state == yyfinal_)
      goto yyacceptlab;

    goto yybackup;

    // Backup.
  yybackup:

    // Try to take a decision without lookahead.
    yyn = yypact_[yystack_[0].state];
    if (yy_pact_value_is_default_ (yyn))
      goto yydefault;

    // Read a lookahead token.
    if (yyempty)
      {
        YYCDEBUG << "Reading a token: ";
        try
          {
            yyla.type = yytranslate_ (yylex (&yyla.value, &yyla.location));
          }
        catch (const syntax_error& yyexc)
          {
            error (yyexc);
            goto yyerrlab1;
          }
        yyempty = false;
      }
    YY_SYMBOL_PRINT ("Next token is", yyla);

    /* If the proper action on seeing token YYLA.TYPE is to reduce or
       to detect an error, take that action.  */
    yyn += yyla.type_get ();
    if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.type_get ())
      goto yydefault;

    // Reduce or error.
    yyn = yytable_[yyn];
    if (yyn <= 0)
      {
        if (yy_table_value_is_error_ (yyn))
          goto yyerrlab;
        yyn = -yyn;
        goto yyreduce;
      }

    // Discard the token being shifted.
    yyempty = true;

    // Count tokens shifted since error; after three, turn off error status.
    if (yyerrstatus_)
      --yyerrstatus_;

    // Shift the lookahead token.
    yypush_ ("Shifting", yyn, yyla);
    goto yynewstate;

  /*-----------------------------------------------------------.
  | yydefault -- do the default action for the current state.  |
  `-----------------------------------------------------------*/
  yydefault:
    yyn = yydefact_[yystack_[0].state];
    if (yyn == 0)
      goto yyerrlab;
    goto yyreduce;

  /*-----------------------------.
  | yyreduce -- Do a reduction.  |
  `-----------------------------*/
  yyreduce:
    yylen = yyr2_[yyn];
    yylhs.state = yy_lr_goto_state_(yystack_[yylen].state, yyr1_[yyn]);
    /* Variants are always initialized to an empty instance of the
       correct type. The default $$=$1 action is NOT applied when using
       variants.  */
      switch (yyr1_[yyn])
    {
      case 44: // relational-op
      case 46: // additive-op
      case 48: // multiplicative-op
        yylhs.value.build< EBinaryOp > ();
        break;

      case 40: // expression
      case 41: // or-op-expr
      case 42: // and-op-expr
      case 43: // relational-op-expr
      case 45: // additive-op-expr
      case 47: // multiplicative-op-expr
      case 49: // atomic-expr
        yylhs.value.build< TExpression* > ();
        break;

      case 36: // where-clause
        yylhs.value.build< TFilterOperator* > ();
        break;

      case 51: // function-expr
        yylhs.value.build< TFunctionExpression* > ();
        break;

      case 52: // function-expr-args
        yylhs.value.build< TFunctionExpression::TArguments > ();
        break;

      case 37: // group-by-clause
        yylhs.value.build< TGroupOperator* > ();
        break;

      case 39: // named-expression
        yylhs.value.build< TNamedExpression > ();
        break;

      case 38: // named-expression-list
        yylhs.value.build< TNamedExpressionList > ();
        break;

      case 33: // head-clause
        yylhs.value.build< TOperator* > ();
        break;

      case 34: // select-clause
        yylhs.value.build< TProjectOperator* > ();
        break;

      case 50: // reference-expr
        yylhs.value.build< TReferenceExpression* > ();
        break;

      case 35: // from-clause
        yylhs.value.build< TScanOperator* > ();
        break;

      case 13: // "identifier"
      case 16: // "YPath literal"
        yylhs.value.build< TStringBuf > ();
        break;

      case 15: // "double literal"
        yylhs.value.build< double > ();
        break;

      case 14: // "integer literal"
        yylhs.value.build< i64 > ();
        break;

      default:
        break;
    }


    // Compute the default @$.
    {
      slice<stack_symbol_type, stack_type> slice (yystack_, yylen);
      YYLLOC_DEFAULT (yylhs.location, slice, yylen);
    }

    // Perform the reduction.
    YY_REDUCE_PRINT (yyn);
    try
      {
        switch (yyn)
          {
  case 2:
    {
            *head = yystack_[0].value.as< TOperator* > ();
        }
    break;

  case 3:
    {
            yystack_[1].value.as< TProjectOperator* > ()->SetSource(yystack_[0].value.as< TScanOperator* > ());
            yylhs.value.as< TOperator* > () = yystack_[1].value.as< TProjectOperator* > ();
        }
    break;

  case 4:
    {
            yystack_[0].value.as< TFilterOperator* > ()->SetSource(yystack_[1].value.as< TScanOperator* > ());
            yystack_[2].value.as< TProjectOperator* > ()->SetSource(yystack_[0].value.as< TFilterOperator* > ());
            yylhs.value.as< TOperator* > () = yystack_[2].value.as< TProjectOperator* > ();
        }
    break;

  case 5:
    {
            yystack_[1].value.as< TFilterOperator* > ()->SetSource(yystack_[2].value.as< TScanOperator* > ());
            yystack_[0].value.as< TGroupOperator* > ()->SetSource(yystack_[1].value.as< TFilterOperator* > ());
            yystack_[3].value.as< TProjectOperator* > ()->SetSource(yystack_[0].value.as< TGroupOperator* > ());
            yylhs.value.as< TOperator* > () = yystack_[3].value.as< TProjectOperator* > ();
        }
    break;

  case 6:
    {
            yystack_[0].value.as< TGroupOperator* > ()->SetSource(yystack_[1].value.as< TScanOperator* > ());
            yystack_[2].value.as< TProjectOperator* > ()->SetSource(yystack_[0].value.as< TGroupOperator* > ());
            yylhs.value.as< TOperator* > () = yystack_[2].value.as< TProjectOperator* > ();
        }
    break;

  case 7:
    {
            yylhs.value.as< TProjectOperator* > () = context->TrackedNew<TProjectOperator>(nullptr);
            yylhs.value.as< TProjectOperator* > ()->Projections().assign(yystack_[0].value.as< TNamedExpressionList > ().begin(), yystack_[0].value.as< TNamedExpressionList > ().end());
        }
    break;

  case 8:
    {
            context->SetTablePath(Stroka(~yystack_[0].value.as< TStringBuf > (), +yystack_[0].value.as< TStringBuf > ()));

            yylhs.value.as< TScanOperator* > () = context->TrackedNew<TScanOperator>();
        }
    break;

  case 9:
    {
            yylhs.value.as< TFilterOperator* > () = context->TrackedNew<TFilterOperator>(nullptr);
            yylhs.value.as< TFilterOperator* > ()->SetPredicate(yystack_[0].value.as< TExpression* > ());
        }
    break;

  case 10:
    {
            yylhs.value.as< TGroupOperator* > () = context->TrackedNew<TGroupOperator>(nullptr);
            yylhs.value.as< TGroupOperator* > ()->GroupItems().assign(yystack_[0].value.as< TNamedExpressionList > ().begin(), yystack_[0].value.as< TNamedExpressionList > ().end());
        }
    break;

  case 11:
    {
            yylhs.value.as< TNamedExpressionList > ().swap(yystack_[2].value.as< TNamedExpressionList > ());
            yylhs.value.as< TNamedExpressionList > ().push_back(yystack_[0].value.as< TNamedExpression > ());
        }
    break;

  case 12:
    {
            yylhs.value.as< TNamedExpressionList > ().push_back(yystack_[0].value.as< TNamedExpression > ());
        }
    break;

  case 13:
    {
            yylhs.value.as< TNamedExpression > () = TNamedExpression(yystack_[0].value.as< TReferenceExpression* > (), yystack_[0].value.as< TReferenceExpression* > ()->GetColumnName());
        }
    break;

  case 14:
    {
            yylhs.value.as< TNamedExpression > () = TNamedExpression(yystack_[2].value.as< TExpression* > (), Stroka(yystack_[0].value.as< TStringBuf > ()));
        }
    break;

  case 15:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TExpression* > (); }
    break;

  case 16:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TBinaryOpExpression>(yylhs.location, EBinaryOp::Or, yystack_[2].value.as< TExpression* > (), yystack_[0].value.as< TExpression* > ());
        }
    break;

  case 17:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TExpression* > (); }
    break;

  case 18:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TBinaryOpExpression>(yylhs.location, EBinaryOp::And, yystack_[2].value.as< TExpression* > (), yystack_[0].value.as< TExpression* > ());
        }
    break;

  case 19:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TExpression* > (); }
    break;

  case 20:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TBinaryOpExpression>(yylhs.location, yystack_[1].value.as< EBinaryOp > (), yystack_[2].value.as< TExpression* > (), yystack_[0].value.as< TExpression* > ());
        }
    break;

  case 21:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TBinaryOpExpression>(yylhs.location, EBinaryOp::And, 
                context->TrackedNew<TBinaryOpExpression>(yylhs.location, EBinaryOp::GreaterOrEqual, yystack_[4].value.as< TExpression* > (), yystack_[2].value.as< TExpression* > ()), 
                context->TrackedNew<TBinaryOpExpression>(yylhs.location, EBinaryOp::LessOrEqual, yystack_[4].value.as< TExpression* > (), yystack_[0].value.as< TExpression* > ()));
        }
    break;

  case 22:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TIntegerLiteralExpression>(yylhs.location, 0);

            for (const TExpression* current : yystack_[1].value.as< TFunctionExpression::TArguments > ()) {
                yylhs.value.as< TExpression* > () = context->TrackedNew<TBinaryOpExpression>(
                    yylhs.location,
                    EBinaryOp::Or,
                    context->TrackedNew<TBinaryOpExpression>(yylhs.location, EBinaryOp::Equal, yystack_[4].value.as< TExpression* > (), current),
                    yylhs.value.as< TExpression* > ());
            }
        }
    break;

  case 23:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TExpression* > (); }
    break;

  case 24:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Equal; }
    break;

  case 25:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::NotEqual; }
    break;

  case 26:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Less; }
    break;

  case 27:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::LessOrEqual; }
    break;

  case 28:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Greater; }
    break;

  case 29:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::GreaterOrEqual; }
    break;

  case 30:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TBinaryOpExpression>(yylhs.location, yystack_[1].value.as< EBinaryOp > (), yystack_[2].value.as< TExpression* > (), yystack_[0].value.as< TExpression* > ());
        }
    break;

  case 31:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TExpression* > (); }
    break;

  case 32:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Plus; }
    break;

  case 33:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Minus; }
    break;

  case 34:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TBinaryOpExpression>(yylhs.location, yystack_[1].value.as< EBinaryOp > (), yystack_[2].value.as< TExpression* > (), yystack_[0].value.as< TExpression* > ());
        }
    break;

  case 35:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TExpression* > (); }
    break;

  case 36:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Multiply; }
    break;

  case 37:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Divide; }
    break;

  case 38:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Modulo; }
    break;

  case 39:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TReferenceExpression* > (); }
    break;

  case 40:
    { yylhs.value.as< TExpression* > () = yystack_[0].value.as< TFunctionExpression* > (); }
    break;

  case 41:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TIntegerLiteralExpression>(yylhs.location, yystack_[0].value.as< i64 > ());
        }
    break;

  case 42:
    {
            yylhs.value.as< TExpression* > () = context->TrackedNew<TDoubleLiteralExpression>(yylhs.location, yystack_[0].value.as< double > ());
        }
    break;

  case 43:
    {
            yylhs.value.as< TExpression* > () = yystack_[1].value.as< TExpression* > ();
        }
    break;

  case 44:
    {
            yylhs.value.as< TReferenceExpression* > () = context->TrackedNew<TReferenceExpression>(yylhs.location, yystack_[0].value.as< TStringBuf > ());
        }
    break;

  case 45:
    {
            yylhs.value.as< TFunctionExpression* > () = context->TrackedNew<TFunctionExpression>(yylhs.location, yystack_[3].value.as< TStringBuf > ());
            yylhs.value.as< TFunctionExpression* > ()->Arguments().assign(yystack_[1].value.as< TFunctionExpression::TArguments > ().begin(), yystack_[1].value.as< TFunctionExpression::TArguments > ().end());
        }
    break;

  case 46:
    {
            yylhs.value.as< TFunctionExpression::TArguments > ().swap(yystack_[2].value.as< TFunctionExpression::TArguments > ());
            yylhs.value.as< TFunctionExpression::TArguments > ().push_back(yystack_[0].value.as< TExpression* > ());
        }
    break;

  case 47:
    {
            yylhs.value.as< TFunctionExpression::TArguments > ().push_back(yystack_[0].value.as< TExpression* > ());
        }
    break;


          default:
            break;
          }
      }
    catch (const syntax_error& yyexc)
      {
        error (yyexc);
        YYERROR;
      }
    YY_SYMBOL_PRINT ("-> $$ =", yylhs);
    yypop_ (yylen);
    yylen = 0;
    YY_STACK_PRINT ();

    // Shift the result of the reduction.
    yypush_ (YY_NULL, yylhs);
    goto yynewstate;

  /*--------------------------------------.
  | yyerrlab -- here on detecting error.  |
  `--------------------------------------*/
  yyerrlab:
    // If not already recovering from an error, report this error.
    if (!yyerrstatus_)
      {
        ++yynerrs_;
        error (yyla.location, yysyntax_error_ (yystack_[0].state,
                                           yyempty ? yyempty_ : yyla.type_get ()));
      }


    yyerror_range[1].location = yyla.location;
    if (yyerrstatus_ == 3)
      {
        /* If just tried and failed to reuse lookahead token after an
           error, discard it.  */

        // Return failure if at end of input.
        if (yyla.type_get () == yyeof_)
          YYABORT;
        else if (!yyempty)
          {
            yy_destroy_ ("Error: discarding", yyla);
            yyempty = true;
          }
      }

    // Else will try to reuse lookahead token after shifting the error token.
    goto yyerrlab1;


  /*---------------------------------------------------.
  | yyerrorlab -- error raised explicitly by YYERROR.  |
  `---------------------------------------------------*/
  yyerrorlab:

    /* Pacify compilers like GCC when the user code never invokes
       YYERROR and the label yyerrorlab therefore never appears in user
       code.  */
    if (false)
      goto yyerrorlab;
    yyerror_range[1].location = yystack_[yylen - 1].location;
    /* $$ was initialized before running the user action.  */
    YY_SYMBOL_PRINT ("Error: discarding", yylhs);
    yylhs.~stack_symbol_type();
    /* Do not reclaim the symbols of the rule whose action triggered
       this YYERROR.  */
    yypop_ (yylen);
    yylen = 0;
    goto yyerrlab1;

  /*-------------------------------------------------------------.
  | yyerrlab1 -- common code for both syntax error and YYERROR.  |
  `-------------------------------------------------------------*/
  yyerrlab1:
    yyerrstatus_ = 3;   // Each real token shifted decrements this.
    {
      stack_symbol_type error_token;
      for (;;)
        {
          yyn = yypact_[yystack_[0].state];
          if (!yy_pact_value_is_default_ (yyn))
            {
              yyn += yyterror_;
              if (0 <= yyn && yyn <= yylast_ && yycheck_[yyn] == yyterror_)
                {
                  yyn = yytable_[yyn];
                  if (0 < yyn)
                    break;
                }
            }

          // Pop the current state because it cannot handle the error token.
          if (yystack_.size () == 1)
            YYABORT;

          yyerror_range[1].location = yystack_[0].location;
          yy_destroy_ ("Error: popping", yystack_[0]);
          yypop_ ();
          YY_STACK_PRINT ();
        }

      yyerror_range[2].location = yyla.location;
      YYLLOC_DEFAULT (error_token.location, yyerror_range, 2);

      // Shift the error token.
      error_token.state = yyn;
      yypush_ ("Shifting", error_token);
    }
    goto yynewstate;

    // Accept.
  yyacceptlab:
    yyresult = 0;
    goto yyreturn;

    // Abort.
  yyabortlab:
    yyresult = 1;
    goto yyreturn;

  yyreturn:
    if (!yyempty)
      yy_destroy_ ("Cleanup: discarding lookahead", yyla);

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYABORT or YYACCEPT.  */
    yypop_ (yylen);
    while (1 < yystack_.size ())
      {
        yy_destroy_ ("Cleanup: popping", yystack_[0]);
        yypop_ ();
      }

    return yyresult;
  }
    catch (...)
      {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack"
                 << std::endl;
        // Do not try to display the values of the reclaimed symbols,
        // as their printer might throw an exception.
        if (!yyempty)
          yy_destroy_ (YY_NULL, yyla);

        while (1 < yystack_.size ())
          {
            yy_destroy_ (YY_NULL, yystack_[0]);
            yypop_ ();
          }
        throw;
      }
  }

  void
  TParser::error (const syntax_error& yyexc)
  {
    error (yyexc.location, yyexc.what());
  }

  // Generate an error message.
  std::string
  TParser::yysyntax_error_ (state_type yystate, symbol_number_type yytoken) const
  {
    std::string yyres;
    // Number of reported tokens (one for the "unexpected", one per
    // "expected").
    size_t yycount = 0;
    // Its maximum.
    enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
    // Arguments of yyformat.
    char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];

    /* There are many possibilities here to consider:
       - If this state is a consistent state with a default action, then
         the only way this function was invoked is if the default action
         is an error action.  In that case, don't check for expected
         tokens because there are none.
       - The only way there can be no lookahead present (in yytoken) is
         if this state is a consistent state with a default action.
         Thus, detecting the absence of a lookahead is sufficient to
         determine that there is no unexpected or expected token to
         report.  In that case, just report a simple "syntax error".
       - Don't assume there isn't a lookahead just because this state is
         a consistent state with a default action.  There might have
         been a previous inconsistent state, consistent state with a
         non-default action, or user semantic action that manipulated
         yyla.  (However, yyla is currently not documented for users.)
       - Of course, the expected token list depends on states to have
         correct lookahead information, and it depends on the parser not
         to perform extra reductions after fetching a lookahead from the
         scanner and before detecting a syntax error.  Thus, state
         merging (from LALR or IELR) and default reductions corrupt the
         expected token list.  However, the list is correct for
         canonical LR with one exception: it will still contain any
         token that will not be accepted due to an error action in a
         later state.
    */
    if (yytoken != yyempty_)
      {
        yyarg[yycount++] = yytname_[yytoken];
        int yyn = yypact_[yystate];
        if (!yy_pact_value_is_default_ (yyn))
          {
            /* Start YYX at -YYN if negative to avoid negative indexes in
               YYCHECK.  In other words, skip the first -YYN actions for
               this state because they are default actions.  */
            int yyxbegin = yyn < 0 ? -yyn : 0;
            // Stay within bounds of both yycheck and yytname.
            int yychecklim = yylast_ - yyn + 1;
            int yyxend = yychecklim < yyntokens_ ? yychecklim : yyntokens_;
            for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
              if (yycheck_[yyx + yyn] == yyx && yyx != yyterror_
                  && !yy_table_value_is_error_ (yytable_[yyx + yyn]))
                {
                  if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
                    {
                      yycount = 1;
                      break;
                    }
                  else
                    yyarg[yycount++] = yytname_[yyx];
                }
          }
      }

    char const* yyformat = YY_NULL;
    switch (yycount)
      {
#define YYCASE_(N, S)                         \
        case N:                               \
          yyformat = S;                       \
        break
        YYCASE_(0, YY_("syntax error"));
        YYCASE_(1, YY_("syntax error, unexpected %s"));
        YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
        YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
        YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
        YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
      }

    // Argument number.
    size_t yyi = 0;
    for (char const* yyp = yyformat; *yyp; ++yyp)
      if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount)
        {
          yyres += yytnamerr_ (yyarg[yyi++]);
          ++yyp;
        }
      else
        yyres += *yyp;
    return yyres;
  }


  const signed char TParser::yypact_ninf_ = -34;

  const signed char TParser::yytable_ninf_ = -14;

  const signed char
  TParser::yypact_[] =
  {
       8,     6,    17,     5,   -34,   -34,     6,   -34,    35,    19,
     -34,    34,    37,    39,     2,    12,    -2,    27,     4,   -34,
     -34,     6,    -5,   -34,    28,     1,     6,    36,     6,     6,
     -34,   -34,   -34,   -34,   -34,   -34,     6,   -34,   -34,     6,
     -34,   -34,   -34,     6,     6,    33,   -34,    -6,   -34,   -34,
       6,     6,    45,   -34,   -34,   -34,    39,     2,    12,   -34,
      -2,   -34,    44,     6,   -34,     6,    37,    19,   -34,     6,
      15,   -34,   -34,   -34
  };

  const unsigned char
  TParser::yydefact_[] =
  {
       0,     0,     0,    44,    41,    42,     0,     2,     0,     7,
      12,     0,    15,    17,    19,    23,    31,    35,    39,    40,
       1,     0,     0,    39,     0,     3,     0,     0,     0,     0,
      26,    27,    24,    25,    28,    29,     0,    32,    33,     0,
      38,    36,    37,     0,     0,     0,    47,     0,    43,     8,
       0,     0,     4,     6,    11,    14,    16,    18,    20,    35,
      30,    34,     0,     0,    45,     0,     9,    10,     5,     0,
       0,    46,    21,    22
  };

  const signed char
  TParser::yypgoto_[] =
  {
     -34,   -34,   -34,   -34,   -34,   -34,     3,     7,    30,   -20,
      -4,    26,    31,   -34,    21,   -34,    20,   -34,   -33,    -1,
     -34,     0
  };

  const signed char
  TParser::yydefgoto_[] =
  {
      -1,     2,     7,     8,    25,    52,    53,     9,    10,    11,
      12,    13,    14,    36,    15,    39,    16,    43,    17,    23,
      19,    47
  };

  const signed char
  TParser::yytable_[] =
  {
      18,    46,    22,    59,   -13,    28,    59,    50,    51,   -13,
      61,    62,     1,    64,    48,    40,    65,    20,    41,     3,
       4,     5,    42,    21,     6,    18,   -13,    30,    31,    32,
      33,    34,    35,    37,    73,    38,    72,    65,    44,    45,
      24,    26,    27,    46,    49,    71,    66,    28,    29,    55,
      18,    63,    51,    69,    56,    68,    54,    58,    67,    60,
      57,     0,     0,    70
  };

  const signed char
  TParser::yycheck_[] =
  {
       1,    21,     6,    36,     0,    10,    39,     6,     7,     5,
      43,    44,     4,    19,    19,    17,    22,     0,    20,    13,
      14,    15,    24,    18,    18,    26,    22,    25,    26,    27,
      28,    29,    30,    21,    19,    23,    69,    22,    11,    12,
       5,    22,     8,    63,    16,    65,    50,    10,     9,    13,
      51,    18,     7,     9,    28,    52,    26,    36,    51,    39,
      29,    -1,    -1,    63
  };

  const unsigned char
  TParser::yystos_[] =
  {
       0,     4,    32,    13,    14,    15,    18,    33,    34,    38,
      39,    40,    41,    42,    43,    45,    47,    49,    50,    51,
       0,    18,    41,    50,     5,    35,    22,     8,    10,     9,
      25,    26,    27,    28,    29,    30,    44,    21,    23,    46,
      17,    20,    24,    48,    11,    12,    40,    52,    19,    16,
       6,     7,    36,    37,    39,    13,    42,    43,    45,    49,
      47,    49,    49,    18,    19,    22,    41,    38,    37,     9,
      52,    40,    49,    19
  };

  const unsigned char
  TParser::yyr1_[] =
  {
       0,    31,    32,    33,    33,    33,    33,    34,    35,    36,
      37,    38,    38,    39,    39,    40,    41,    41,    42,    42,
      43,    43,    43,    43,    44,    44,    44,    44,    44,    44,
      45,    45,    46,    46,    47,    47,    48,    48,    48,    49,
      49,    49,    49,    49,    50,    51,    52,    52
  };

  const unsigned char
  TParser::yyr2_[] =
  {
       0,     2,     2,     2,     3,     4,     3,     1,     2,     2,
       2,     3,     1,     1,     3,     1,     3,     1,     3,     1,
       3,     5,     5,     1,     1,     1,     1,     1,     1,     1,
       3,     1,     1,     1,     3,     1,     1,     1,     1,     1,
       1,     1,     1,     3,     1,     4,     3,     1
  };



  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a yyntokens_, nonterminals.
  const char*
  const TParser::yytname_[] =
  {
  "\"end of stream\"", "error", "$undefined", "\"lexer failure\"",
  "StrayWillParseQuery", "\"keyword `FROM`\"", "\"keyword `WHERE`\"",
  "\"keyword `GROUP BY`\"", "\"keyword `AS`\"", "\"keyword `AND`\"",
  "\"keyword `OR`\"", "\"keyword `BETWEEN`\"", "\"keyword `IN`\"",
  "\"identifier\"", "\"integer literal\"", "\"double literal\"",
  "\"YPath literal\"", "\"`%`\"", "\"`(`\"", "\"`)`\"", "\"`*`\"",
  "\"`+`\"", "\"`,`\"", "\"`-`\"", "\"`/`\"", "\"`<`\"", "\"`<=`\"",
  "\"`=`\"", "\"`!=`\"", "\"`>`\"", "\"`>=`\"", "$accept", "head",
  "head-clause", "select-clause", "from-clause", "where-clause",
  "group-by-clause", "named-expression-list", "named-expression",
  "expression", "or-op-expr", "and-op-expr", "relational-op-expr",
  "relational-op", "additive-op-expr", "additive-op",
  "multiplicative-op-expr", "multiplicative-op", "atomic-expr",
  "reference-expr", "function-expr", "function-expr-args", YY_NULL
  };

#if YT_QL_YYDEBUG
  const unsigned short int
  TParser::yyrline_[] =
  {
       0,   122,   122,   129,   134,   140,   147,   156,   164,   173,
     181,   189,   194,   201,   205,   212,   217,   221,   226,   230,
     235,   239,   245,   257,   262,   264,   266,   268,   270,   272,
     277,   281,   286,   288,   293,   297,   302,   304,   306,   311,
     313,   315,   319,   323,   330,   337,   345,   350
  };

  // Print the state stack on the debug stream.
  void
  TParser::yystack_print_ ()
  {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator
           i = yystack_.begin (),
           i_end = yystack_.end ();
         i != i_end; ++i)
      *yycdebug_ << ' ' << i->state;
    *yycdebug_ << std::endl;
  }

  // Report on the debug stream that the rule \a yyrule is going to be reduced.
  void
  TParser::yy_reduce_print_ (int yyrule)
  {
    unsigned int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1
               << " (line " << yylno << "):" << std::endl;
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
      YY_SYMBOL_PRINT ("   $" << yyi + 1 << " =",
                       yystack_[(yynrhs) - (yyi + 1)]);
  }
#endif // YT_QL_YYDEBUG

  // Symbol number corresponding to token number t.
  inline
  TParser::token_number_type
  TParser::yytranslate_ (int t)
  {
    static
    const token_number_type
    translate_table[] =
    {
     0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,    17,     2,     2,
      18,    19,    20,    21,    22,    23,     2,    24,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      25,    27,    29,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     3,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     4,
       1,     2,     5,     6,     7,     8,     9,    10,    11,    12,
      13,    14,    15,    16,    26,    28,    30
    };
    const unsigned int user_token_number_max_ = 1016;
    const token_number_type undef_token_ = 2;

    if (static_cast<int>(t) <= yyeof_)
      return yyeof_;
    else if (static_cast<unsigned int> (t) <= user_token_number_max_)
      return translate_table[t];
    else
      return undef_token_;
  }

} } // NYT::NQueryClient


namespace NYT {
namespace NQueryClient {

void TParser::error(const location_type& location, const std::string& message)
{
    THROW_ERROR_EXCEPTION("Error while parsing query: %s", message.c_str())
        << TErrorAttribute("query_range", Sprintf("%d-%d", location.first, location.second));
}

} // namespace NQueryClient
} // namespace NYT

