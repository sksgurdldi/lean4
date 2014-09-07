/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <string>
#include <functional>
#include <limits>
#include <algorithm>
#include <vector>
#include "util/sstream.h"
#include "util/exception.h"
#include "util/sexpr/option_declarations.h"
#include "util/bitap_fuzzy_search.h"
#include "kernel/instantiate.h"
#include "library/aliases.h"
#include "library/type_util.h"
#include "library/unifier.h"
#include "library/opaque_hints.h"
#include "library/scoped_ext.h"
#include "library/tactic/goal.h"
#include "frontends/lean/server.h"
#include "frontends/lean/parser.h"

// #define LEAN_SERVER_DIAGNOSTIC

#if defined(LEAN_SERVER_DIAGNOSTIC)
#define DIAG(CODE) CODE
#else
#define DIAG(CODE)
#endif

#define LEAN_FUZZY_MAX_ERRORS        3
#define LEAN_FUZZY_MAX_ERRORS_FACTOR 3
#define LEAN_FIND_CONSUME_IMPLICIT   true // lean will add metavariables for implicit arguments when printing the type of declarations in FINDP and FINDG

namespace lean {
server::file::file(std::istream & in, std::string const & fname):m_fname(fname) {
    for (std::string line; std::getline(in, line);) {
        m_lines.push_back(line);
    }
}

void server::file::replace_line(unsigned line_num, std::string const & new_line) {
    lock_guard<mutex> lk(m_lines_mutex);
    while (line_num >= m_lines.size())
        m_lines.push_back("");
    #if 0
    std::string const & old_line = m_lines[line_num];
    unsigned i = 0;
    while (i < old_line.size() && i < new_line.size() && old_line[i] == new_line[i])
        i++;
    #endif
    m_info.block_new_info(true);
    #if 0
    // It turns out that is not a good idea to try to "keep" some of the information.
    // The info_manager will accumulate conflicting information and confuse the frontend.
    // Example: open a theorem, delete the proof, then type "and", then continue with "and.elim"
    // Now, the info_manager contains the "and" and "and.elim" type.
    m_info.invalidate_line_col(line_num+1, i);
    #endif
    m_info.invalidate_line(line_num+1);
    m_lines[line_num] = new_line;
}

void server::file::insert_line(unsigned line_num, std::string const & new_line) {
    lock_guard<mutex> lk(m_lines_mutex);
    m_info.block_new_info(true);
    m_info.insert_line(line_num+1);
    while (line_num >= m_lines.size())
        m_lines.push_back("");
    m_lines.push_back("");
    lean_assert(m_lines.size() >= line_num+1);
    unsigned i = m_lines.size();
    while (i > line_num) {
        --i;
        m_lines[i] = m_lines[i-1];
    }
    m_lines[line_num] = new_line;
}

void server::file::remove_line(unsigned line_num) {
    lock_guard<mutex> lk(m_lines_mutex);
    m_info.block_new_info(true);
    m_info.remove_line(line_num+1);
    if (line_num >= m_lines.size())
        return;
    lean_assert(!m_lines.empty());
    for (unsigned i = line_num; i < m_lines.size()-1; i++)
        m_lines[i] = m_lines[i+1];
    m_lines.pop_back();
}

void server::file::show(std::ostream & out, bool valid) {
    lock_guard<mutex> lk(m_lines_mutex);
    for (unsigned i = 0; i < m_lines.size(); i++) {
        if (valid) {
            if (m_info.is_invalidated(i+1))
                out << "*";
            else
                out << " ";
            out << " ";
        }
        out << m_lines[i] << "\n";
    }
}

/**
   \brief Return index i <= m_snapshots.size() s.t.
      * forall j < i, m_snapshots[j].m_line < line
      * forall i <= j < m_snapshots.size(),  m_snapshots[j].m_line >= line
*/
unsigned server::file::find(unsigned line_num) {
    unsigned low  = 0;
    unsigned high = m_snapshots.size();
    while (true) {
        lean_assert(low <= high);
        if (low == high)
            return low;
        unsigned mid = low + ((high - low)/2);
        lean_assert(low <= mid && mid < high);
        lean_assert(mid < m_snapshots.size());
        snapshot const & s = m_snapshots[mid];
        if (s.m_line < line_num) {
            low  = mid+1;
        } else {
            high = mid;
        }
    }
}

/** \brief Copy lines [starting_from, m_lines.size()) to block and return the total number of lines */
unsigned server::file::copy_to(std::string & block, unsigned starting_from) {
    unsigned num_lines = m_lines.size();
    for (unsigned j = starting_from; j < num_lines; j++) {
        block += m_lines[j];
        block += '\n';
    }
    return num_lines;
}

server::worker::worker(environment const & env, io_state const & ios, definition_cache & cache):
    m_empty_snapshot(env, ios.get_options()),
    m_cache(cache),
    m_todo_line_num(0),
    m_todo_options(ios.get_options()),
    m_terminate(false),
    m_thread([=]() {
            io_state _ios(ios);
            while (!m_terminate) {
                file_ptr todo_file;
                unsigned todo_line_num = 0;
                options  todo_options;
                // wait for next task
                while (!m_terminate) {
                    unique_lock<mutex> lk(m_todo_mutex);
                    if (m_todo_file) {
                        todo_file    = m_todo_file;
                        todo_line_num = m_todo_line_num;
                        todo_options = m_todo_options;
                        break;
                    } else {
                        m_todo_cv.wait(lk);
                    }
                }
                // extract block of code and snapshot from todo_file
                reset_interrupt();
                bool worker_interrupted = false;
                if (m_terminate)
                    break;
                DIAG(std::cerr << "processing '" << todo_file->get_fname() << "'\n";)
                std::string block;
                unsigned    num_lines;
                snapshot    s;
                {
                    lean_assert(todo_file);
                    lock_guard<mutex> lk(todo_file->m_lines_mutex);
                    unsigned i = todo_file->find(todo_line_num);
                    todo_file->m_snapshots.resize(i);
                    s = i == 0 ? m_empty_snapshot : todo_file->m_snapshots[i-1];
                    lean_assert(s.m_line > 0);
                    todo_file->m_info.block_new_info(false);
                    todo_file->m_info.set_processed_upto(s.m_line);
                    num_lines = todo_file->copy_to(block, s.m_line - 1);
                }
                if (m_terminate)
                    break;
                // parse block of code with respect to snapshot
                try {
                    std::istringstream strm(block);
                    #if defined(LEAN_SERVER_DIAGNOSTIC)
                    std::shared_ptr<output_channel> out1(new stderr_channel());
                    std::shared_ptr<output_channel> out2(new stderr_channel());
                    #else
                    std::shared_ptr<output_channel> out1(new string_output_channel());
                    std::shared_ptr<output_channel> out2(new string_output_channel());
                    #endif
                    io_state tmp_ios(_ios, out1, out2);
                    tmp_ios.set_options(join(s.m_options, _ios.get_options()));
                    bool use_exceptions  = false;
                    unsigned num_threads = 1;
                    parser p(s.m_env, tmp_ios, strm, todo_file->m_fname.c_str(), use_exceptions, num_threads,
                             s.m_lds, s.m_eds, s.m_line, &todo_file->m_snapshots, &todo_file->m_info);
                    p.set_cache(&m_cache);
                    p();
                } catch (interrupted &) {
                    worker_interrupted = true;
                } catch (exception & ex) {
                    DIAG(std::cerr << "worker exception: " << ex.what() << "\n";)
                }
                if (!m_terminate && !worker_interrupted) {
                    DIAG(std::cerr << "finished '" << todo_file->get_fname() << "'\n";)
                    unique_lock<mutex> lk(m_todo_mutex);
                    if (m_todo_file == todo_file && m_last_file == todo_file && m_todo_line_num == todo_line_num) {
                        m_todo_line_num = num_lines + 1;
                        m_todo_file    = nullptr;
                        m_todo_cv.notify_all();
                    }
                }
            }
        }) {}

server::worker::~worker() {
    m_terminate = true;
    request_interrupt();
    m_thread.join();
}

void server::worker::request_interrupt() {
    m_todo_cv.notify_all();
    m_thread.request_interrupt();
}

void server::worker::wait() {
    while (true) {
        unique_lock<mutex> lk(m_todo_mutex);
        if (!m_todo_file)
            break;
        m_todo_cv.wait(lk);
    }
}

void server::worker::set_todo(file_ptr const & f, unsigned line_num, options const & o) {
    lock_guard<mutex> lk(m_todo_mutex);
    if (m_last_file != f || line_num < m_todo_line_num)
        m_todo_line_num = line_num;
    m_todo_file    = f;
    m_last_file    = f;
    m_todo_options = o;
    m_todo_cv.notify_all();
}

server::server(environment const & env, io_state const & ios, unsigned num_threads):
    m_env(env), m_ios(ios), m_out(ios.get_regular_channel().get_stream()),
    m_num_threads(num_threads), m_empty_snapshot(m_env, m_ios.get_options()),
    m_worker(env, ios, m_cache) {
#if !defined(LEAN_MULTI_THREAD)
    lean_unreachable();
#endif
}

server::~server() {
}

void server::interrupt_worker() {
    m_worker.request_interrupt();
}

static std::string g_load("LOAD");
static std::string g_visit("VISIT");
static std::string g_replace("REPLACE");
static std::string g_insert("INSERT");
static std::string g_remove("REMOVE");
static std::string g_info("INFO");
static std::string g_set("SET");
static std::string g_eval("EVAL");
static std::string g_wait("WAIT");
static std::string g_clear_cache("CLEAR_CACHE");
static std::string g_echo("ECHO");
static std::string g_options("OPTIONS");
static std::string g_show("SHOW");
static std::string g_valid("VALID");
static std::string g_sleep("SLEEP");
static std::string g_findp("FINDP");
static std::string g_findg("FINDG");

static bool is_command(std::string const & cmd, std::string const & line) {
    return line.compare(0, cmd.size(), cmd) == 0;
}

static std::string & ltrim(std::string & s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
    return s;
}

static std::string & rtrim(std::string & s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
    return s;
}

static std::string & trim(std::string & s) {
    return ltrim(rtrim(s));
}

void server::process_from(unsigned line_num) {
    m_worker.set_todo(m_file, line_num, m_ios.get_options());
}

void server::load_file(std::string const & fname, bool error_if_nofile) {
    interrupt_worker();
    std::ifstream in(fname);
    if (in.bad() || in.fail()) {
        if (error_if_nofile) {
            m_out << "-- ERROR failed to open file '" << fname << "'" << std::endl;
        } else {
            m_file.reset(new file(in, fname));
            m_file_map.insert(mk_pair(fname, m_file));
        }
    } else {
        m_file.reset(new file(in, fname));
        m_file_map.insert(mk_pair(fname, m_file));
        process_from(0);
    }
}

void server::visit_file(std::string const & fname) {
    interrupt_worker();
    auto it = m_file_map.find(fname);
    if (it == m_file_map.end()) {
        bool error_if_nofile = false;
        load_file(fname, error_if_nofile);
    } else {
        m_file = it->second;
        process_from(0);
    }
}

void server::read_line(std::istream & in, std::string & line) {
    if (!std::getline(in, line))
        throw exception("unexpected end of input");
}

void consume_spaces(std::string const & data, unsigned & i) {
    while (i < data.size() && std::isspace(data[i])) {
        i++;
    }
}

unsigned consume_num(std::string const & data, unsigned & i) {
    unsigned sz = data.size();
    if (i >= sz || !std::isdigit(data[i]))
        throw exception("numeral expected");
    unsigned long long r = 0;
    while (i < sz && std::isdigit(data[i])) {
        r = r*10 + (data[i] - '0');
        if (r > std::numeric_limits<unsigned>::max())
            throw exception("numeral is too big to fit in a 32-bit machine unsigned integer");
        i++;
    }
    return r;
}

void check_line_num(unsigned line_num) {
    if (line_num == 0)
        throw exception("line numbers are indexed from 1");
}

// Given a line of the form "cmd line_num", return the line_num
unsigned server::get_line_num(std::string const & line, std::string const & cmd) {
    std::string data = line.substr(cmd.size());
    unsigned i = 0;
    consume_spaces(data, i);
    unsigned r = consume_num(data, i);
    check_line_num(r);
    return r;
}

pair<unsigned, optional<unsigned>> server::get_line_opt_col_num(std::string const & line, std::string const & cmd) {
    std::string data = line.substr(cmd.size());
    unsigned i  = 0;
    consume_spaces(data, i);
    unsigned line_num = consume_num(data, i);
    check_line_num(line_num);
    consume_spaces(data, i);
    if (i == data.size())
        return mk_pair(line_num, optional<unsigned>());
    unsigned colnum = consume_num(data, i);
    return mk_pair(line_num, optional<unsigned>(colnum));
}

pair<unsigned, unsigned> server::get_line_col_num(std::string const & line, std::string const & cmd) {
    auto r = get_line_opt_col_num(line, cmd);
    if (r.second)
        return mk_pair(r.first, *r.second);
    else
        return mk_pair(r.first, 0);
}

void server::check_file() {
    if (!m_file)
        throw exception("no file has been loaded/visited");
}

void server::replace_line(unsigned line_num, std::string const & new_line) {
    interrupt_worker();
    check_file();
    m_file->replace_line(line_num, new_line);
    process_from(line_num);
}

void server::insert_line(unsigned line_num, std::string const & new_line) {
    interrupt_worker();
    check_file();
    m_file->insert_line(line_num, new_line);
    process_from(line_num);
}

void server::remove_line(unsigned line_num) {
    interrupt_worker();
    check_file();
    m_file->remove_line(line_num);
    process_from(line_num);
}

void server::set_option(std::string const & line) {
    std::string cmd = "set_option ";
    cmd += line;
    std::istringstream strm(cmd);
    m_out << "-- BEGINSET" << std::endl;
    try {
        parser p(m_env, m_ios, strm, "SET_command", true);
        p();
        m_ios.set_options(p.ios().get_options());
    } catch (exception & ex) {
        m_out << ex.what() << std::endl;
    }
    m_out << "-- ENDSET" << std::endl;
}

void server::show_info(unsigned line_num, optional<unsigned> const & col_num) {
    check_file();
    m_out << "-- BEGININFO";
    if (m_file->infom().is_invalidated(line_num))
        m_out << " STALE";
    if (line_num >= m_file->infom().get_processed_upto())
        m_out << " NAY";
    m_out << std::endl;
    m_file->infom().display(m_env, m_ios, line_num, col_num);
    m_out << "-- ENDINFO" << std::endl;
}

void server::eval_core(environment const & env, options const & o, std::string const & line) {
    std::istringstream strm(line);
    io_state ios(m_ios, o);
    m_out << "-- BEGINEVAL" << std::endl;
    try {
        parser p(env, ios, strm, "EVAL_command", true);
        p();
    } catch (exception & ex) {
        m_out << ex.what() << std::endl;
    }
    m_out << "-- ENDEVAL" << std::endl;
}

void server::eval(std::string const & line) {
    if (!m_file) {
        eval_core(m_env, m_ios.get_options(), line);
    } else if (auto p = m_file->infom().get_final_env_opts()) {
        eval_core(p->first, join(p->second, m_ios.get_options()), line);
    } else {
        eval_core(m_env, m_ios.get_options(), line);
    }
}

void server::show_options() {
    m_out << "-- BEGINOPTIONS" << std::endl;
    options const & o = m_ios.get_options();
    option_declarations const & decls = get_option_declarations();
    for (auto it = decls.begin(); it != decls.end(); it++) {
        option_declaration const & d = it->second;
        m_out << "-- " << d.get_name() << "|" << d.kind() << "|";
        d.display_value(m_out, o);
        m_out << "|" << d.get_description() << "\n";
    }
    m_out << "-- ENDOPTIONS" << std::endl;
}

void server::show(bool valid) {
    check_file();
    m_out << "-- BEGINSHOW" << std::endl;
    m_file->show(m_out, valid);
    m_out << "-- ENDSHOW" << std::endl;
}

void server::display_decl(name const & short_name, name const & long_name, environment const & env, options const & o) {
    declaration const & d = env.get(long_name);
    io_state_stream out   = regular(env, m_ios).update_options(o);
    expr type = d.get_type();
    if (LEAN_FIND_CONSUME_IMPLICIT) {
        while (true) {
            if (!is_pi(type))
                break;
            if (!binding_info(type).is_implicit())
                break;
            std::string q("?");
            q += binding_name(type).to_string();
            expr m = mk_constant(name(q.c_str()));
            type   = instantiate(binding_body(type), m);
        }
    }
    out << short_name << "|" << mk_pair(flatten(out.get_formatter()(type)), o) << "\n";
}

optional<name> is_uniquely_aliased(environment const & env, name const & n) {
    if (auto it = is_expr_aliased(env, n))
        if (length(get_expr_aliases(env, *it)) == 1)
            return it;
    return optional<name>();
}

/** \brief Return an (atomic) name if \c n can be referenced by this atomic
    name in the given environment. */
optional<name> is_essentially_atomic(environment const & env, name const & n) {
    if (n.is_atomic())
        return optional<name>(n);
    list<name> const & ns_list = get_namespaces(env);
    for (name const & ns : ns_list) {
        if (is_prefix_of(ns, n)) {
            auto n_prime = n.replace_prefix(ns, name());
            if (n_prime.is_atomic())
                return optional<name>(n_prime);
            break;
        }
    }
    if (auto it = is_uniquely_aliased(env, n))
        if (it->is_atomic())
            return it;
    return optional<name>();
}

void server::display_decl(name const & d, environment const & env, options const & o) {
    // using namespace override resolution rule
    list<name> const & ns_list = get_namespaces(env);
    for (name const & ns : ns_list) {
        if (is_prefix_of(ns, d)) {
            display_decl(d.replace_prefix(ns, name()), d, env, o);
            return;
        }
    }
    // if the alias is unique use it
    if (auto it = is_uniquely_aliased(env, d)) {
        display_decl(*it, d, env, o);
    } else {
        display_decl(d, d, env, o);
    }
}

unsigned get_fuzzy_match_max_errors(unsigned prefix_sz) {
    unsigned r = (prefix_sz / LEAN_FUZZY_MAX_ERRORS_FACTOR);
    if (r > LEAN_FUZZY_MAX_ERRORS)
        return LEAN_FUZZY_MAX_ERRORS;
    return r;
}

void server::find_pattern(unsigned line_num, std::string const & pattern) {
    check_file();
    m_out << "-- BEGINFINDP";
    unsigned upto = m_file->infom().get_processed_upto();
    optional<pair<environment, options>> env_opts = m_file->infom().get_closest_env_opts(line_num);
    if (!env_opts) {
        m_out << " NAY" << std::endl;
        m_out << "-- ENDFINDP" << std::endl;
        return;
    }
    if (upto < line_num)
        m_out << " STALE";
    environment const & env = env_opts->first;
    options opts            = env_opts->second;
    token_table const & tt  = get_token_table(env);
    if (is_token(tt, pattern.c_str())) {
        // we ignore patterns that match commands, keywords, and tokens.
        m_out << "\n-- ENDFINDP" << std::endl;
        return;
    }
    opts = join(opts, m_ios.get_options());
    m_out << std::endl;
    unsigned max_errors = get_fuzzy_match_max_errors(pattern.size());
    std::vector<pair<std::string, name>> selected;
    bitap_fuzzy_search matcher(pattern, max_errors);
    env.for_each_declaration([&](declaration const & d) {
            std::string text = d.get_name().to_string();
            if (auto it = is_essentially_atomic(env, d.get_name())) {
                std::string it_str = it->to_string();
                // if pattern "perfectly" matches beginning of declaration name, we just display d on the top of the list
                if (it_str.compare(0, pattern.size(), pattern) == 0) {
                    display_decl(*it, d.get_name(), env, opts);
                    return;
                }
            }
            if (matcher.match(text))
                selected.emplace_back(text, d.get_name());
        });
    unsigned sz = selected.size();
    if (sz == 1) {
        display_decl(selected[0].second, env, opts);
    } else if (sz > 1) {
        std::vector<pair<std::string, name>> next_selected;
        for (unsigned k = 0; k <= max_errors; k++) {
            bitap_fuzzy_search matcher(pattern, k);
            for (auto const & s : selected) {
                if (matcher.match(s.first)) {
                    display_decl(s.second, env, opts);
                } else {
                    next_selected.push_back(s);
                }
            }
            std::swap(selected, next_selected);
            next_selected.clear();
        }
    }
    m_out << "-- ENDFINDP" << std::endl;
}

void consume_pos_neg_strs(std::string const & filters, buffer<std::string> & pos_names, buffer<std::string> & neg_names) {
    unsigned i  = 0;
    unsigned sz = filters.size();
    std::string val;
    while (true) {
        consume_spaces(filters, i);
        if (i == sz)
            return;
        if (filters[i] == '+' || filters[i] == '-') {
            bool pos = filters[i] == '+';
            i++;
            val.clear();
            while (i < sz && !std::isspace(filters[i])) {
                val += filters[i];
                i++;
            }
            if (val.empty())
                throw exception("invalid empty filter");
            name n = string_to_name(val);
            if (!n.is_atomic())
                throw exception("invalid filter, atomic name expected");
            if (pos)
                pos_names.push_back(n.to_string());
            else
                neg_names.push_back(n.to_string());
        } else {
            throw exception("invalid filter, '+' or '-' expected");
        }
    }
}

bool is_part_of(std::string const & p, name n) {
    while (true) {
        if (n.is_string()) {
            std::string s(n.get_string());
            if (s.find(p) != std::string::npos)
                return true;
        }
        if (n.is_atomic() || n.is_anonymous())
            return false;
        n = n.get_prefix();
    }
}

bool match_type(type_checker & tc, expr const & meta, expr const & expected_type, declaration const & d) {
    name_generator ngen = tc.mk_ngen();
    goal g(meta, expected_type);
    buffer<level> ls;
    unsigned num_ls = length(d.get_univ_params());
    for (unsigned i = 0; i < num_ls; i++)
        ls.push_back(mk_meta_univ(ngen.next()));
    expr dt        = instantiate_type_univ_params(d, to_list(ls.begin(), ls.end()));
    unsigned num_e = get_expect_num_args(tc, expected_type);
    unsigned num_d = get_expect_num_args(tc, dt);
    if (num_e > num_d)
        return false;
    for (unsigned i = 0; i < num_d - num_e; i++) {
        dt        = tc.whnf(dt).first;
        expr meta = g.mk_meta(ngen.next(), binding_domain(dt));
        dt        = instantiate(binding_body(dt), meta);
    }
    // Remark: we ignore declarations where the resultant type is of the form
    // (?M ...) because they unify with almost everything. They produce a lot of noise in the output.
    // Perhaps we should have an option to enable/disable this kind of declaration. For now, we
    // just ingore them.
    if (is_meta(dt))
        return false; // matches anything
    auto r = unify(tc.env(), dt, expected_type, tc.mk_ngen(), true);
    return static_cast<bool>(r.pull());
}

static name g_tmp_prefix = name::mk_internal_unique_name();
void server::find_goal_matches(unsigned line_num, unsigned col_num, std::string const & filters) {
    buffer<std::string> pos_names, neg_names;
    consume_pos_neg_strs(filters, pos_names, neg_names);
    m_out << "-- BEGINFINDG";
    optional<pair<environment, options>> env_opts = m_file->infom().get_closest_env_opts(line_num);
    if (!env_opts) {
        m_out << " NAY" << std::endl;
        m_out << "-- ENDFINDG" << std::endl;
        return;
    }
    if (line_num >= m_file->infom().get_processed_upto())
        m_out << " NAY";
    m_out << std::endl;
    environment const & env = env_opts->first;
    options const & opts    = env_opts->second;
    name_generator ngen(g_tmp_prefix);
    std::unique_ptr<type_checker> tc = mk_type_checker_with_hints(env, ngen, true);
    if (auto meta = m_file->infom().get_meta_at(line_num, col_num)) {
    if (is_meta(*meta)) {
    if (auto type = m_file->infom().get_type_at(line_num, col_num)) {
        env.for_each_declaration([&](declaration const & d) {
                if (std::all_of(pos_names.begin(), pos_names.end(),
                                [&](std::string const & pos) { return is_part_of(pos, d.get_name()); }) &&
                    std::all_of(neg_names.begin(), neg_names.end(),
                                [&](std::string const & neg) { return !is_part_of(neg, d.get_name()); }) &&
                    match_type(*tc.get(), *meta, *type, d)) {
                    if (optional<name> alias = is_expr_aliased(env, d.get_name()))
                        display_decl(*alias, d.get_name(), env, opts);
                    else
                        display_decl(d.get_name(), d.get_name(), env, opts);
                }
            });
    }}}
    m_out << "-- ENDFINDG" << std::endl;
}

bool server::operator()(std::istream & in) {
    for (std::string line; std::getline(in, line);) {
        try {
            if (is_command(g_load, line)) {
                std::string fname = line.substr(g_load.size());
                trim(fname);
                load_file(fname);
            } else if (is_command(g_visit, line)) {
                std::string fname = line.substr(g_visit.size());
                trim(fname);
                visit_file(fname);
            } else if (is_command(g_echo, line)) {
                std::string str = line.substr(g_echo.size());
                m_out << "--" << str << "\n";
            } else if (is_command(g_replace, line)) {
                unsigned line_num = get_line_num(line, g_replace);
                read_line(in, line);
                replace_line(line_num-1, line);
            } else if (is_command(g_insert, line)) {
                unsigned line_num = get_line_num(line, g_insert);
                read_line(in, line);
                insert_line(line_num-1, line);
            } else if (is_command(g_remove, line)) {
                unsigned line_num = get_line_num(line, g_remove);
                remove_line(line_num-1);
            } else if (is_command(g_info, line)) {
                auto line_col = get_line_opt_col_num(line, g_info);
                show_info(line_col.first, line_col.second);
            } else if (is_command(g_set, line)) {
                read_line(in, line);
                set_option(line);
            } else if (is_command(g_eval, line)) {
                read_line(in, line);
                eval(line);
            } else if (is_command(g_clear_cache, line)) {
                m_cache.clear();
            } else if (is_command(g_options, line)) {
                show_options();
            } else if (is_command(g_wait, line)) {
                m_worker.wait();
            } else if (is_command(g_show, line)) {
                show(false);
            } else if (is_command(g_valid, line)) {
                show(true);
            } else if (is_command(g_sleep, line)) {
                unsigned ms = get_line_num(line, g_sleep);
                chrono::milliseconds d(ms);
                this_thread::sleep_for(d);
            } else if (is_command(g_findp, line)) {
                unsigned line_num = get_line_num(line, g_findp);
                read_line(in, line);
                if (line.size() > 63)
                    line.resize(63);
                find_pattern(line_num, line);
            } else if (is_command(g_findg, line)) {
                pair<unsigned, unsigned> line_col_num = get_line_col_num(line, g_findg);
                read_line(in, line);
                find_goal_matches(line_col_num.first, line_col_num.second, line);
            } else {
                throw exception(sstream() << "unexpected command line: " << line);
            }
        } catch (exception & ex) {
            m_out << "-- ERROR " << ex.what() << std::endl;
        }
    }
    return true;
}
}
