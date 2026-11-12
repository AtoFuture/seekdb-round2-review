#include "sql.hpp"
#include "minivectordb/value.hpp"
#include <set>
namespace minivectordb::sql {
  namespace {
    class Parser {
      std::vector<Token> tokens;
      std::size_t at=0;
      const Token& peek()const {
        return tokens.at(at);
      }
      bool take(std::string_view s) {
        if((peek().kind==TokenKind::Word||peek().kind==TokenKind::Symbol)&&peek().text==s) {
          ++at;
          return true;
        }
        return false;
      }
      void need(std::string_view s) {
        if(!take(s))throw Error(ErrorCode::Syntax,"expected "+std::string(s));
      }
      std::string name() {
        if(peek().kind!=TokenKind::Word&&peek().kind!=TokenKind::Identifier)throw Error(ErrorCode::Syntax,"expected identifier");
        if(peek().text.empty())throw Error(ErrorCode::Syntax,"empty identifier");
        return tokens[at++].text;
      }
      std::uint64_t integer() {
        if(peek().kind!=TokenKind::Number)throw Error(ErrorCode::Syntax,"expected integer");
        try {
          auto n=parse_bigint(tokens[at++].text);
          if(n<0)throw Error(ErrorCode::Syntax,"expected nonnegative integer");
          return static_cast<std::uint64_t>(n);
        }
        catch(const std::invalid_argument&e) {
          throw Error(ErrorCode::Syntax,e.what());
        }
      }
      Literal literal() {
        Literal v;
        if(peek().kind==TokenKind::Number) {
          v.kind=Literal::Number;
          v.text=tokens[at++].text;
        }
        else if(peek().kind==TokenKind::String) {
          v.kind=Literal::String;
          v.text=tokens[at++].text;
        }
        else if(take("[")) {
          v.kind=Literal::Vector;
          if(!take("]")) {
            do {
              if(peek().kind!=TokenKind::Number)throw Error(ErrorCode::Syntax,"expected VECTOR component");
              v.components.push_back(tokens[at++].text);
            }
            while(take(","));
            need("]");
          }
        }
        else throw Error(ErrorCode::Syntax,"expected constant literal");
        return v;
      }
      Expr expr() {
        Expr e;
        if(peek().kind==TokenKind::Word&&peek().text=="l2_distance") {
          ++at;
          need("(");
          e.column=name();
          need(",");
          e.query=literal();
          need(")");
        }
        else e.column=name();
        return e;
      }
      void where(Statement&s) {
        if(take("where")) {
          do {
            Predicate p;
            p.column=name();
            if(peek().kind!=TokenKind::Symbol||std::set<std::string> {
              "=","<","<=",">",">="
            }
            .count(peek().text)==0)throw Error(ErrorCode::Syntax,"unsupported predicate operator");
            p.op=tokens[at++].text;
            p.value=literal();
            s.predicates.push_back(std::move(p));
          }
          while(take("and"));
        }
      }
      void create(Statement&s) {
        if(take("vector")) {
          s.kind=Statement::CreateIndex;
          need("index"); s.index_name=name(); need("on"); s.table=name();
          need("("); s.index_column=name(); need(")");
          return;
        }
        need("table");
        s.table=name();
        s.schema.name=s.table;
        need("(");
        std::optional<std::string> primary;
        do {
          if(take("primary")) {
            need("key");
            need("(");
            auto pk=name();
            need(")");
            if(primary)throw Error(ErrorCode::Schema,"multiple primary keys");
            primary=pk;
          }
          else {
            Column c;
            c.name=name();
            if(take("bigint"))c.type=ColumnType::Bigint;
            else if(take("varchar"))c.type=ColumnType::Varchar;
            else if(take("vector"))c.type=ColumnType::Vector;
            else throw Error(ErrorCode::Syntax,"unsupported column type");
            if(c.type!=ColumnType::Bigint) {
              need("(");
              auto n=integer();
              if(n==0||n>UINT32_MAX)throw Error(ErrorCode::Schema,"invalid type extent");
              c.extent=static_cast<std::uint32_t>(n);
              need(")");
            }
            if(take("primary")) {
              need("key");
              if(primary)throw Error(ErrorCode::Schema,"multiple primary keys");
              primary=c.name;
            }
            s.schema.columns.push_back(std::move(c));
          }
        }
        while(take(","));
        need(")");
        if(!primary)throw Error(ErrorCode::Schema,"table requires a primary key");
        s.schema.primary=column_index(s.schema,*primary);
        validate_schema(s.schema);
      }
      public:
      explicit Parser(std::string_view sql):tokens(lex(sql)) {
      }
      Statement run() {
        Statement s;
        if(take("create")) {
          s.kind=Statement::Create;
          create(s);
        }
        else if(take("insert")) {
          s.kind=Statement::Insert;
          need("into");
          s.table=name();
          if(take("(")) {
            do {
              s.insert_columns.push_back(name());
            }
            while(take(","));
            need(")");
          }
          need("values");
          do {
            need("(");
            std::vector<Literal> row;
            do {
              row.push_back(literal());
            }
            while(take(","));
            need(")");
            s.values.push_back(std::move(row));
          }
          while(take(","));
        }
        else if(take("select")) {
          s.kind=Statement::Select;
          if(take("*"))s.star=true;
          else do {
            auto e=expr();
            if(take("as"))e.alias=name();
            s.projection.push_back(std::move(e));
          }
          while(take(","));
          need("from");
          s.table=name();
          where(s);
          if(take("order")) {
            need("by");
            do {
              Order o;
              o.expression=expr();
              if(take("desc"))o.descending=true;
              else take("asc");
              s.order.push_back(std::move(o));
            }
            while(take(","));
          }
          if(take("limit"))s.limit=integer();
        }
        else if(take("update")) {
          s.kind=Statement::Update;
          s.table=name();
          need("set");
          do {
            auto c=name();
            need("=");
            s.assignments.emplace_back(c,literal());
          }
          while(take(","));
          where(s);
        }
        else if(take("delete")) {
          s.kind=Statement::Delete;
          need("from");
          s.table=name();
          where(s);
        }
        else if(take("begin"))s.kind=Statement::Begin;
        else if(take("commit"))s.kind=Statement::Commit;
        else if(take("rollback"))s.kind=Statement::Rollback;
        else throw Error(ErrorCode::Syntax,"unsupported SQL statement");
        take(";");
        if(peek().kind!=TokenKind::End)throw Error(ErrorCode::Syntax,"unexpected trailing SQL or unsupported clause");
        return s;
      }
    };
  }
  Statement parse(std::string_view s) {
    return Parser(s).run();
  }
}
