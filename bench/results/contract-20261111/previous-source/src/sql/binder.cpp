#include "minivectordb/profile.hpp"
#include "sql.hpp"
#include "minivectordb/value.hpp"
#include <charconv>
#include <cmath>
#include <set>
#include <limits>
#include <locale.h>
#include <cstdlib>
#include <new>
namespace minivectordb::sql {
  std::size_t column_index(const Schema&s,std::string_view name) {
    for(std::size_t i=0;i<s.columns.size();++i)if(s.columns[i].name==name)return i;
    throw Error(ErrorCode::Schema,"unknown column: "+std::string(name));
  }
  namespace {
    double real(std::string_view text) {
      if(!text.empty()&&text.front()=='+')text.remove_prefix(1);
      double result=0;
      auto[end,error]=std::from_chars(text.data(),text.data()+text.size(),result);
      if(error==std::errc::result_out_of_range && end==text.data()+text.size()) {
        // from_chars reports both overflow and underflow with this code.
        // Values below binary64 range are still valid binary32 signed zeros.
        // Use a private immutable C locale, never the caller's global locale.
        struct NumericLocale {
          locale_t value=::newlocale(LC_NUMERIC_MASK,"C",nullptr);
          NumericLocale() {if(!value)throw std::bad_alloc();}
          ~NumericLocale() {::freelocale(value);}
        };
        static const NumericLocale locale;
        const std::string terminated(text);
        char* tail=nullptr;
        const auto tiny=::strtod_l(terminated.c_str(),&tail,locale.value);
        if(tail==terminated.c_str()+terminated.size() && std::isfinite(tiny) &&
           std::abs(tiny)<std::numeric_limits<double>::min()) return tiny;
      }
      if(error!=std::errc{} ||end!=text.data()+text.size()||!std::isfinite(result))
        throw Error(ErrorCode::Type,"invalid finite VECTOR component");
      return result;
    }
    std::vector<std::string> vector_components(const Literal&v) {
      if(v.kind==Literal::Vector)return v.components;
      if(v.kind!=Literal::String)throw Error(ErrorCode::Type,"expected VECTOR literal");
      auto t=lex(v.text);
      std::size_t i=0;
      auto take=[&](std::string_view s) {
        if(t.at(i).kind==TokenKind::Symbol&&t[i].text==s) {
          ++i;
          return true;
        }
        return false;
      };
      if(!take("["))throw Error(ErrorCode::Type,"invalid quoted VECTOR");
      std::vector<std::string> parts;
      if(!take("]")) {
        do {
          if(t.at(i).kind!=TokenKind::Number)throw Error(ErrorCode::Type,"invalid quoted VECTOR component");
          parts.push_back(t[i++].text);
        }
        while(take(","));
        if(!take("]"))throw Error(ErrorCode::Type,"invalid quoted VECTOR");
      }
      if(t.at(i).kind!=TokenKind::End)throw Error(ErrorCode::Type,"trailing quoted VECTOR data");
      return parts;
    }
    Cell bind_predicate_literal(const Literal& value, const Column& column) {
      if(column.type!=ColumnType::Varchar)return bind_literal(value,column);
      if(value.kind!=Literal::String)throw Error(ErrorCode::Type,"expected VARCHAR literal");
      // Comparison constants are not stored in the column, so its width does not apply.
      try {
        validate_text(value.text,std::numeric_limits<std::size_t>::max());
      } catch(const std::invalid_argument& error) {
        throw Error(ErrorCode::Type,error.what());
      }
      return value.text;
    }
    BoundExpr bind_expr(const Expr&e,const Schema&s) {
      BoundExpr b;
      b.column=column_index(s,e.column);
      b.label=e.alias.empty()?(e.query?"l2_distance":e.column):e.alias;
      if(e.query) {
        if(s.columns[b.column].type!=ColumnType::Vector)throw Error(ErrorCode::Type,"L2_DISTANCE requires VECTOR column");
        b.query=std::get<std::vector<float>>(bind_literal(*e.query,s.columns[b.column]));
      }
      return b;
    }
  }
  Cell bind_literal(const Literal&v,const Column&c) {
    try {
      if(c.type==ColumnType::Bigint) {
        if(v.kind!=Literal::Number)throw Error(ErrorCode::Type,"expected BIGINT literal");
        return parse_bigint(v.text);
      }
      if(c.type==ColumnType::Varchar) {
        if(v.kind!=Literal::String)throw Error(ErrorCode::Type,"expected VARCHAR literal");
        validate_text(v.text,c.extent);
        return v.text;
      }
      auto components=vector_components(v);
      std::vector<double> values;
      values.reserve(components.size());
      for(const auto&part:components)values.push_back(real(part));
      return make_vector(values,c.extent);
    }
    catch(const std::invalid_argument&e) {
      throw Error(ErrorCode::Type,e.what());
    }
  }
  Bound bind(const Statement&s,const Schema&schema) {
    Bound b;
    for(const auto&p:s.predicates) {
      auto index=column_index(schema,p.column);
      if(schema.columns[index].type==ColumnType::Vector)throw Error(ErrorCode::Type,"VECTOR predicates are unsupported");
      auto value=bind_predicate_literal(p.value,schema.columns[index]);
      if(index==schema.primary&&p.op=="=")b.primary_lookup=std::get<std::int64_t>(value);
      b.predicates.push_back( {
        index,p.op,std::move(value)
      });
    }
    if(s.kind==Statement::Select) {
      if(s.star) {
        for(std::size_t i=0;i<schema.columns.size();++i)b.projection.push_back( {
          i, {
          },schema.columns[i].name
        });
      }
      else for(const auto&e:s.projection)b.projection.push_back(bind_expr(e,schema));
      for(const auto&o:s.order) {
        std::optional<BoundExpr> alias;
        if(!o.expression.query) {
          for(std::size_t i=0;i<s.projection.size();++i)if(!s.projection[i].alias.empty()&&s.projection[i].alias==o.expression.column) {
            if(alias)throw Error(ErrorCode::Schema,"ambiguous ORDER BY alias");
            alias=b.projection[i];
          }
        }
        auto expr=alias?*alias:bind_expr(o.expression,schema);
        if(!expr.query&&schema.columns[expr.column].type==ColumnType::Vector)throw Error(ErrorCode::Type,"cannot order VECTOR directly");
        b.order.push_back( {
          std::move(expr),o.descending
        });
      }
    }
    if(s.kind==Statement::Insert) {
      std::vector<std::size_t> indexes;
      std::set<std::size_t> seen;
      if(s.insert_columns.empty()) {
        for(std::size_t i=0;i<schema.columns.size();++i)indexes.push_back(i);
      }
      else {
        for(const auto&name:s.insert_columns) {
          auto index=column_index(schema,name);
          if(!seen.insert(index).second)throw Error(ErrorCode::Schema,"duplicate INSERT column");
          indexes.push_back(index);
        }
        if(indexes.size()!=schema.columns.size())throw Error(ErrorCode::Type,"all columns must be provided; NULL and defaults are unsupported");
      }
      std::set<std::int64_t> keys;
      for(const auto&values:s.values) {
        if(values.size()!=indexes.size())throw Error(ErrorCode::Type,"INSERT column count mismatch");
        Row row(schema.columns.size());
        for(std::size_t i=0;i<values.size();++i)row[indexes[i]]=bind_literal(values[i],schema.columns[indexes[i]]);
        auto pk=std::get<std::int64_t>(row[schema.primary]);
        if(!keys.insert(pk).second)throw Error(ErrorCode::Constraint,"duplicate primary key within INSERT");
        b.insert_rows.push_back(std::move(row));
      }
    }
    std::set<std::size_t> assigned;
    for(const auto&[name,value]:s.assignments) {
      auto index=column_index(schema,name);
      if(index==schema.primary)throw Error(ErrorCode::Constraint,"primary key updates are unsupported");
      if(!assigned.insert(index).second)throw Error(ErrorCode::Schema,"duplicate assignment");
      b.assignments.emplace_back(index,bind_literal(value,schema.columns[index]));
    }
    return b;
  }
  int compare_cells(const Cell&a,const Cell&b) {
    if(a.index()!=b.index())throw Error(ErrorCode::Type,"incompatible comparison types");
    if(auto p=std::get_if<std::int64_t>(&a)) {
      auto q=std::get<std::int64_t>(b);
      return *p<q?-1:(*p>q?1:0);
    }
    if(auto p=std::get_if<std::string>(&a))return compare_text(*p,std::get<std::string>(b));
    if(auto p=std::get_if<double>(&a)) {
      auto q=std::get<double>(b);
      return *p<q?-1:(*p>q?1:0);
    }
    throw Error(ErrorCode::Type,"unsupported comparison");
  }
  bool matches(const Row&r,const std::vector<BoundPredicate>&predicates) {
    for(const auto&p:predicates) {
      int cmp=compare_cells(r[p.column],p.value);
      if((p.op=="="&&cmp!=0)||(p.op=="<"&&cmp>=0)||(p.op=="<="&&cmp>0)||(p.op==">"&&cmp<=0)||(p.op==">="&&cmp<0))return false;
    }
    return true;
  }
  double squared_distance(const std::vector<float>&a,const std::vector<float>&b) {
    profile::detail::Scope timer(&profile::Snapshot::vector_distance);
    double sum=0;
    for(std::size_t i=0;i<a.size();++i) {
      double d=static_cast<double>(a[i])-b[i];
      sum+=d*d;
    }
    return sum;
  }
}
