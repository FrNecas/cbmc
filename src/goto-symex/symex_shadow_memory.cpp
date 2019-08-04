/*******************************************************************\

Module: Symex Shadow Memory Instrumentation

Author: Peter Schrammel

\*******************************************************************/

/// \file
/// Symex Shadow Memory Instrumentation

#include "goto_symex.h"

#include <util/arith_tools.h>
#include <util/base_type.h>
#include <util/c_types.h>
#include <util/cprover_prefix.h>
#include <util/find_symbols.h>
#include <util/fresh_symbol.h>
#include <util/pointer_predicates.h>
#include <util/prefix.h>
#include <util/invariant.h>
#include <util/message.h>
#include <util/pointer_offset_size.h>
#include <util/replace_expr.h>
#include <util/source_location.h>
#include <util/std_expr.h>

#include <langapi/language_util.h>

#include <goto-programs/goto_model.h>

static irep_idt get_field_name(const exprt &string_expr)
{
  if(string_expr.id() == ID_typecast)
    return get_field_name(to_typecast_expr(string_expr).op());
  else if(string_expr.id() == ID_address_of)
    return get_field_name(to_address_of_expr(string_expr).object());
  else if(string_expr.id() == ID_index)
    return get_field_name(to_index_expr(string_expr).array());
  else if(string_expr.id() == ID_string_constant)
  {
    return string_expr.get(ID_value);
  }
  else
    UNREACHABLE;
}

void goto_symext::initialize_rec(
  const namespacet &ns,
  goto_symex_statet &state,
  const exprt &expr,
  std::map<irep_idt, typet> &fields)
{
  typet type = ns.follow(expr.type());
  if(type.id() == ID_array && !shadow_per_object)
  {
    exprt size_expr = to_array_type(type).size();
    if(!size_expr.is_constant())
    {
      log.error() << "constant array size expected:\n"
                  << size_expr.pretty()
                  << messaget::eom;
      throw 0;
    }
    mp_integer array_size;
    to_integer(to_constant_expr(size_expr), array_size);
    for(mp_integer index = 0; index < array_size; ++index)
    {
      initialize_rec(
        ns,
        state,
        index_exprt(expr, from_integer(index, signed_long_int_type())),
        fields);
    }
  }
  else if(type.id() == ID_struct && !shadow_per_object)
  {
    for(const auto &component : to_struct_type(type).components())
    {
      initialize_rec(ns, state, member_exprt(expr, component), fields);
    }
  }

  if(shadow_per_object || (type.id() != ID_array && type.id() != ID_struct))
  {
    for(const auto &field_pair : fields)
    {
      symbol_exprt field =
        add_field(ns, state, address_of_exprt(expr), field_pair.first, fields);
      code_assignt code_assign(
        field, from_integer(mp_integer(0), field.type()));
      symex_assign(state, code_assign);

      log.debug() << "initialize field " << id2string(field.get_identifier())
                  << " for " << from_expr(ns, "", address_of_exprt(expr))
                  << messaget::eom;
    }
  }
}

symbol_exprt goto_symext::add_field(
  const namespacet &ns,
  goto_symex_statet &state,
  const exprt &expr,
  const irep_idt &field_name,
  std::map<irep_idt, typet> &fields)
{
  auto &addresses = address_fields[field_name];
  symbolt &new_symbol = get_fresh_aux_symbol(
    fields.at(field_name),
    id2string(state.source.function_id),
    from_expr(ns, "", expr) + "." + id2string(field_name),
    state.source.pc->source_location,
    ID_C,
    state.symbol_table);

  addresses.push_back(
    std::pair<exprt, symbol_exprt>(expr, new_symbol.symbol_expr()));
  return new_symbol.symbol_expr();
}

bool goto_symext::filter_by_value_set(
  const value_setst::valuest &value_set,
  const exprt &address)
{
  if(address.id() != ID_address_of)
    return false;

  const auto &expr2 = to_address_of_expr(address).object();
  if(expr2.id() != ID_symbol)
    return false;

  for(const auto &e : value_set)
  {
    if(e.id() != ID_object_descriptor)
      continue;

    const auto &expr1 = to_object_descriptor_expr(e).object();
    if(expr1.id() != ID_symbol)
      continue;

    if(to_symbol_expr(expr1).get_identifier() ==
       to_symbol_expr(expr2).get_identifier())
    {
      return true;
    }
  }
  return false;
}

void goto_symext::symex_set_field(
  const namespacet &ns,
  goto_symex_statet &state,
  const code_function_callt &code_function_call)
{
  INVARIANT(
    code_function_call.arguments().size() == 3,
    CPROVER_PREFIX "set_field requires 3 arguments");
  irep_idt field_name = get_field_name(code_function_call.arguments()[1]);

  exprt expr = code_function_call.arguments()[0];
  typet expr_type = expr.type();
  DATA_INVARIANT(
    expr.type().id() == ID_pointer,
    "shadow memory requires a pointer expression");

  exprt value = code_function_call.arguments()[2];
  
  log.debug() << "set_field: " << id2string(field_name) << " for "
              << from_expr(ns, "", expr) << " to " << from_expr(ns, "", value)
              << messaget::eom;

  if(shadow_per_object)
  {
    expr = pointer_object(expr);
    log.debug() << "set_field: corresponds to "
                << from_expr(ns, "", expr)
                << messaget::eom;
  }
  
  INVARIANT(
    address_fields.count(field_name) == 1,
    id2string(field_name) + " should exist");
  const auto &addresses = address_fields.at(field_name);
  const exprt &rhs = value;
  exprt lhs = nil_exprt();
  size_t mux_size = 0;
  value_setst::valuest value_set;
  state.value_set.get_value_set(expr, value_set, ns);

  bool has_entry = false;
  for(const auto &address_pair : addresses)
  {
    if(filter_by_value_set(value_set, address_pair.first))
    {
      has_entry = true;
      break;
    }
  }
  
  for(const auto &address_pair : addresses)
  {
    const exprt &address = address_pair.first;

    if(has_entry && !filter_by_value_set(value_set, address))
      continue;

    if(
      expr_type == address.type() ||
      to_pointer_type(expr_type).get_width() ==
        to_pointer_type(address.type()).get_width())
    {
      const exprt &field = address_pair.second;
      exprt cond = equal_exprt(
        address, typecast_exprt::conditional_cast(expr, address.type()));
      // do_simplify(cond);
      if(!cond.is_false())
      {
        mux_size++;
        if(lhs.is_nil())
        {
          lhs = address_of_exprt(field);
        }
        else
        {
          lhs = if_exprt(cond, address_of_exprt(field), lhs);
        }
      }
    }
  }
  log.debug() << "set_field: " << mux_size << messaget::eom;
  lhs = dereference_exprt(lhs);
  symex_assign(
    state,
    code_assignt(lhs, typecast_exprt::conditional_cast(rhs, lhs.type())));
}

void goto_symext::symex_get_field(
  const namespacet &ns,
  goto_symex_statet &state,
  const code_function_callt &code_function_call)
{
  INVARIANT(
    code_function_call.arguments().size() == 2,
    CPROVER_PREFIX "get_field requires 2 arguments");
  irep_idt field_name = get_field_name(code_function_call.arguments()[1]);

  exprt expr = code_function_call.arguments()[0];
  typet expr_type = expr.type();
  DATA_INVARIANT(
    expr_type.id() == ID_pointer,
    "shadow memory requires a pointer expression");

  log.debug() << "get_field: " << id2string(field_name) << " for "
              << from_expr(ns, "", expr) << messaget::eom;

  if(shadow_per_object)
  {
    expr = pointer_object(expr);
    log.debug() << "get_field: corresponds to "
                << from_expr(ns, "", expr)
                << messaget::eom;
  }

  INVARIANT(
    address_fields.count(field_name) == 1,
    id2string(field_name) + " should exist");
  const auto &addresses = address_fields.at(field_name);
  // Should actually be fields.at(field_name)
  symbol_exprt lhs(CPROVER_PREFIX "get_field#return_value", signedbv_typet(32));
  exprt rhs = nil_exprt();
  size_t mux_size = 0;
  value_setst::valuest value_set;
  state.value_set.get_value_set(expr, value_set, ns);

  bool has_entry = false;
  for(const auto &address_pair : addresses)
  {
    if(filter_by_value_set(value_set, address_pair.first))
    {
      has_entry = true;
      break;
    }
  }

  for(const auto &address_pair : addresses)
  {
    const exprt &address = address_pair.first;

    if(has_entry && !filter_by_value_set(value_set, address))
      continue;

    if(
      expr_type == address.type() ||
      to_pointer_type(expr_type).get_width() ==
        to_pointer_type(address.type()).get_width())
    {
      const exprt &field = address_pair.second;
      exprt cond = equal_exprt(
        address, typecast_exprt::conditional_cast(expr, address.type()));
      // do_simplify(cond);
      if(!cond.is_false())
      {
        mux_size++;
        if(rhs.is_nil())
        {
          rhs = typecast_exprt::conditional_cast(field, lhs.type());
        }
        else
        {
          rhs = if_exprt(
            cond, typecast_exprt::conditional_cast(field, lhs.type()), rhs);
        }
      }
    }
  }
  symex_assign(
    state,
    code_assignt(lhs, typecast_exprt::conditional_cast(rhs, lhs.type())));
  log.debug() << "get_field: " << mux_size << messaget::eom;
}

void goto_symext::symex_field_static_init(
  const namespacet &ns,
  goto_symex_statet &state,
  const code_assignt &code_assign)
{
  if(state.source.function_id != CPROVER_PREFIX "initialize")
    return;

  find_symbols_sett identifiers = find_symbol_identifiers(code_assign.lhs());
  if(identifiers.size() != 1)
    return;

  const irep_idt &identifier = *identifiers.begin();
  if(has_prefix(id2string(identifier), CPROVER_PREFIX))
    return;

  const symbolt &symbol = ns.lookup(identifier);

  if(symbol.is_auxiliary || !symbol.is_static_lifetime)
    return;
  if(id2string(symbol.name).find("__cs_") != std::string::npos)
    return;

  const typet &type = symbol.type;
  log.debug() << "global memory " << id2string(symbol.name) << " of type "
              << from_type(ns, "", type) << messaget::eom;

  initialize_rec(ns, state, symbol.symbol_expr(), global_fields);
}

void goto_symext::symex_field_local_init(
  const namespacet &ns,
  goto_symex_statet &state,
  const symbol_exprt &expr)
{
  const symbolt &symbol = ns.lookup(expr.get_identifier());

  if(symbol.is_auxiliary)
    return;
  if(id2string(symbol.name).find("__cs_") != std::string::npos)
    return;

  const typet &type = expr.type();
  log.debug() << "local memory " << id2string(expr.get_identifier())
              << " of type " << from_type(ns, "", type) << messaget::eom;

  initialize_rec(ns, state, expr, local_fields);
}

void goto_symext::symex_field_dynamic_init(
  const namespacet &ns,
  goto_symex_statet &state,
  const exprt &expr,
  const mp_integer &size)
{
  log.debug() << "dynamic memory of type " << from_type(ns, "", expr.type())
              << " and " << size << " element(s)" << messaget::eom;

  if(shadow_per_object)
  {
    initialize_rec(
      ns,
      state,
      expr,
      global_fields);
  }
  else
  {
    for(mp_integer index = 0; index < size; ++index)
    {
      initialize_rec(
        ns,
        state,
        dereference_exprt(
          plus_exprt(expr, from_integer(index, signed_long_int_type()))),
        global_fields);
    }
  }
}

std::pair<std::map<irep_idt, typet>, std::map<irep_idt, typet>>
goto_symext::preprocess_field_decl(
  goto_modelt &goto_model,
  message_handlert &message_handler)
{
  std::map<irep_idt, typet> global_fields;
  std::map<irep_idt, typet> local_fields;
  namespacet ns(goto_model.symbol_table);

  // get declarations
  Forall_goto_functions(f_it, goto_model.goto_functions)
  {
    goto_programt &goto_program = f_it->second.body;
    Forall_goto_program_instructions(target, goto_program)
    {
      if(!target->is_function_call())
        continue;

      const code_function_callt &code_function_call =
        to_code_function_call(target->code);
      const exprt &function = code_function_call.function();

      if(function.id() != ID_symbol)
        continue;

      const irep_idt &identifier = to_symbol_expr(function).get_identifier();

      if(identifier == CPROVER_PREFIX "field_decl_global")
      {
        convert_field_decl(
          ns, message_handler, code_function_call, global_fields);
        target->make_skip();
      }
      else if(identifier == CPROVER_PREFIX "field_decl_local")
      {
        convert_field_decl(
          ns, message_handler, code_function_call, local_fields);
        target->make_skip();
      }
    }
  }
  return std::make_pair(global_fields, local_fields);
}

void goto_symext::convert_field_decl(
  const namespacet &ns,
  message_handlert &message_handler,
  const code_function_callt &code_function_call,
  std::map<irep_idt, typet> &fields)
{
  INVARIANT(
    code_function_call.arguments().size() == 2,
    CPROVER_PREFIX "field_decl requires 2 arguments");
  irep_idt field_name = get_field_name(code_function_call.arguments()[0]);

  exprt expr = code_function_call.arguments()[1];

  messaget log(message_handler);
  log.debug() << "declare " << id2string(field_name) << " of type "
              << from_type(ns, "", expr.type()) << messaget::eom;

  // record field type
  fields[field_name] = expr.type();
}
