#pragma once
#include "minivectordb/row_codec.hpp"
#include <optional>
namespace minivectordb::sql {
  enum class TokenKind {
    Word,Identifier,Number,String,Symbol,End
  };
  struct Token {
    TokenKind kind;
    std::string text;
  };
  std::vector<Token> lex(std::string_view);
  struct Literal {
    enum Kind {
      Number,String,Vector
    }
    kind=Number;
    std::string text;
    std::vector<std::string> components;
  };
  struct Expr {
    std::string column;
    std::optional<Literal> query;
    std::string alias;
  };
  struct Predicate {
    std::string column,op;
    Literal value;
  };
  struct Order {
    Expr expression;
    bool descending=false;
  };
  struct Statement {
    enum Kind {
      Create,CreateIndex,Insert,Select,Update,Delete,Begin,Commit,Rollback
    }
    kind;
    std::string table;
    std::string index_name,index_column;
    Schema schema;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<Literal>> values;
    std::vector<Expr> projection;
    bool star=false;
    std::vector<Predicate> predicates;
    std::vector<Order> order;
    std::optional<std::uint64_t> limit;
    std::vector<std::pair<std::string,Literal>> assignments;
  };
  Statement parse(std::string_view);
  std::size_t column_index(const Schema&,std::string_view);
  Cell bind_literal(const Literal&,const Column&,bool fast_vector=true);
  struct BoundExpr {
    std::size_t column;
    std::optional<std::vector<float>> query;
    std::string label;
  };
  struct BoundPredicate {
    std::size_t column;
    std::string op;
    Cell value;
  };
  struct BoundOrder {
    BoundExpr expression;
    bool descending;
  };
  struct Bound {
    std::vector<BoundExpr> projection;
    std::vector<BoundPredicate> predicates;
    std::vector<BoundOrder> order;
    std::vector<std::pair<std::size_t,Cell>> assignments;
    std::vector<Row> insert_rows;
    std::optional<std::int64_t> primary_lookup;
  };
  Bound bind(const Statement&,const Schema&);
  int compare_cells(const Cell&,const Cell&);
  bool matches(const Row&,const std::vector<BoundPredicate>&);
  double squared_distance(const std::vector<float>&,const std::vector<float>&);
}
