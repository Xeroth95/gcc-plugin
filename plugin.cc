#include <string>
#include <string_view>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cassert>

#include <gcc-plugin.h>
#include <plugin-version.h>

#include <context.h>
#include <tree.h>
#include <tree-pass.h>
#include <print-tree.h>
#include <line-map.h>

#include <gimple.h>
#include <gimple-expr.h>
#include <gimple-iterator.h>
#include <gimple-walk.h>
#include <gimple-pretty-print.h>

#include <diagnostic-core.h>

#include <basic-block.h>

#include <tree-pass.h>

#include <stringpool.h>

int plugin_is_GPL_compatible;

namespace {
	plugin_info my_info {
		.version = "Hallo",
		.help = " no help ",
	};
};

extern gcc::context *g;

class tso : public gimple_opt_pass
{
public:
	tso(const pass_data& pd) : gimple_opt_pass(pd, g) {}
	tso* clone() override { return this; }
	unsigned int execute(function* f) override;
};

int plugin_init(plugin_name_args *plugin_info, plugin_gcc_version *version)
{
	pass_data new_pass_data = {
		.type = GIMPLE_PASS,
		.name = "tracker-pass",
		.optinfo_flags = OPTGROUP_NONE,
		.tv_id = TV_NONE,
		.properties_required = PROP_gimple_lcf,
		.properties_provided = 0,
		.properties_destroyed = 0,
		.todo_flags_start = 0,
		.todo_flags_finish = 0
	};

	register_pass_info tracker_pass = {
		.pass = new tso(new_pass_data),
		.reference_pass_name = "cfg",
		.ref_pass_instance_number = 1, // only the first pass
		.pos_op = PASS_POS_INSERT_AFTER,

	};

	if (!plugin_default_version_check(version, &gcc_version)) {
		printf("Bad gcc version...\n");
		return 1;
	}

	register_callback(plugin_info->base_name, PLUGIN_INFO, nullptr, &my_info);

	register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP,
			  nullptr, &tracker_pass);

	printf("Plugin initialized ...\n");
	return 0;
}


static std::string
strip_path(const std::string& path)
{
	size_t i = path.rfind('/');

	return i == std::string::npos ?
			path : path.substr(i + 1, std::string::npos);
}

static void
debug_func(function *f)
{
	tree decl = f->decl;

	const std::string top_path{main_input_filename};
	const std::string& top_file = strip_path(top_path);
	const std::string path{DECL_SOURCE_FILE(decl)};
	const std::string& file = strip_path(path);
	const std::string ident{fndecl_name(decl)};


	size_t sz = ident.size() + file.size() + top_file.size() + 3;
	size_t n = sz > 70 ? 10 : 80 - sz;

	std::cerr << std::string(n, '-') << " " << top_file << ":" << file <<
			":" << ident << std::endl;
}

static void
debug_stmt(/* const */ gimple* st)
{
	enum gimple_code code = gimple_code(st);

	std::cerr << "- " << code;

	std::cerr << " | ";
	debug_gimple_stmt(st);
}

// use ident if we know that its an IDENTIFIER_NODE
using ident = tree;

struct variable {
	tree id;
	location_t loc;
};

struct track_status {
	location_t last_changed;
	bool tracked;
};


struct simple_text_label : public range_label {
	const char* str;
	simple_text_label(const char* str) : str{str} {}
	label_text get_text(unsigned) const {
		return label_text::borrow(str);
	}
};

simple_text_label PREVIOUSLY_TRACKED{"previously tracked here"};
simple_text_label NEWLY_TRACKED{"newly tracked here"};
simple_text_label PREVIOUSLY_UNTRACKED{"previously untracked here"};
simple_text_label NEWLY_UNTRACKED{"newly untracked here"};

struct block_info {
	std::vector<variable> newly_tracked;   // first encounter = tracked
	std::vector<variable> newly_untracked; // first encounter = untracked
	std::unordered_map<tree, track_status> seen;

	bool empty() const {
		return newly_tracked.size() == 0 &&
			newly_untracked.size() == 0;
	}

	void track(location_t loc, tree var) {
		auto found = seen.find(var);

		if (found == seen.end()) {
			newly_tracked.push_back({var, loc});
			track_status s = {
				.last_changed = loc,
				.tracked = true,
			};
			seen.try_emplace(var, s);
			return;
		}

		auto& status = found->second;
		if (status.tracked) {

			rich_location location{
				nullptr, loc, &NEWLY_TRACKED
			};

			location.add_range(status.last_changed,
					   SHOW_LINES_WITHOUT_RANGE,
					   &PREVIOUSLY_TRACKED
					  );


			error_at(&location,
				 "trying to track currently tracked variable");
			return;
		} else {
			status.tracked = true;
		}
	}

	void untrack(location_t loc, ident var) {
		auto found = seen.find(var);

		if (found == seen.end()) {
			newly_untracked.push_back({var, loc});
			track_status s = {
				.last_changed = loc,
				.tracked = false,
			};
			seen.try_emplace(var, s);
			return;
		}

		auto& status = found->second;

		if (!status.tracked) {
			rich_location location{
				nullptr, loc, &NEWLY_UNTRACKED
			};

			location.add_range(status.last_changed,
					   SHOW_LINES_WITHOUT_RANGE,
					   &PREVIOUSLY_UNTRACKED
					  );

			error_at(&location,
				 "trying to untrack currently untracked variable");
			return;
		} else {
			status.tracked = false;
		}
	}
};

struct context {
	ident track, untrack;
};

bool is_variable(tree t)
{
	switch (TREE_CODE(t)) {
	case FIELD_DECL: // structure field (?)
	case VAR_DECL: // variable
		//case CONST_DECL:
	case PARM_DECL: // (function) parameter
		// case TYPE_DECL:
	case RESULT_DECL: // no idea
		return true;
	default:
		return false;
	}
}

void start_tracking(block_info& current,
		    gimple* st)
{
	location_t loc = gimple_location(st);
	unsigned num_args = gimple_call_num_args(st);
	if (num_args != 1) {
		error_at(loc, "track accepts exactly one argument");
		return;
	}

	tree arg = gimple_call_arg(st, 0);

	// artificial = temporary that was inserted by compiler
	if (!is_variable(arg) || DECL_ARTIFICIAL(arg)) {
		error_at(loc, "can only track variables");
		return;
	}

	current.track(loc, arg);

	auto name = DECL_NAME(arg);
	printf("tracking %s\n", IDENTIFIER_POINTER(name));
}

void stop_tracking(block_info& current, gimple* st)
{
	location_t loc = gimple_location(st);
	unsigned num_args = gimple_call_num_args(st);

	if (num_args != 1) {
		error_at(loc, "untrack accepts exactly one argument");
		return;
	}

	tree arg = gimple_call_arg(st, 0);

	if (!is_variable(arg) || DECL_ARTIFICIAL(arg)) {

		printf("code = %d (%d); artificial = %s\n",
		       TREE_CODE(arg), VAR_DECL, DECL_ARTIFICIAL(arg) ? "yes" : "no"
		      );

		error_at(loc, "can only untrack variables");
		return;
	}

	current.untrack(loc, arg);
	auto name = DECL_NAME(arg);
	printf("untracking %s\n", IDENTIFIER_POINTER(name));
}

void handle_gimple(block_info& current, gimple* st, const context& ctx)
{
	debug_stmt(st);
	auto code = gimple_code(st);

	if (code == GIMPLE_CALL) {
		// search for track/untrack



		tree fdecl = gimple_call_fndecl(st); // TREE_CODE() = FUNCTION_DECL

		ident name = DECL_NAME(fdecl);

		printf("name = %s\n", IDENTIFIER_POINTER(name));
		if (name == ctx.track) {
			printf("found track\n");

			start_tracking(current, st);

		} else if (name == ctx.untrack) {
			printf("found untrack\n");
			stop_tracking(current, st);
		}



		// tree fn = gimple_call_fn(st); // TREE_CODE() = ADDR_EXPR ??

		// if (num_args == 1) {
		// 	warning_at(loc, OPT_Wunused_result,
		// 		   "found %qD",
		// 		   fdecl
		// 		  );
		// 	//printf("One found\n");
		// }
	}

	// walk_stmt_info wi;
	// walk_gimple_op(st, +[]{}, &wi);

	// todo: iterate over the gimpl for subgimpls and recurse into them

}

std::unordered_map<basic_block, block_info> build_block_infos(
	basic_block start,
	basic_block end,
	const context& ctx)
{
	basic_block current = start;

	std::unordered_map<basic_block, block_info> block_infos{};
	while (current != end) {
		block_info& info = block_infos[current];


		gimple_stmt_iterator si;
		printf("---  start block %d --- \n", current->index);
		for (si = gsi_start_bb(current);
		     !gsi_end_p(si);
		     gsi_next(&si)) {
			gimple* stmt = gsi_stmt(si);

			handle_gimple(info, stmt, ctx);
		}
		printf("---  end block %d --- \n", current->index);

		current = current->next_bb;
	}

	// give an empty block to end
	block_infos.emplace(end, block_info{});

	return block_infos;
}

struct state {
	std::unordered_map<tree, location_t> tracked;
	basic_block block{nullptr};
};

void violations_recurse(std::vector<state> stack,
			const std::unordered_map<basic_block, block_info>& infos,
			basic_block current)
{
	assert(stack.size() > 0);
	auto& last = stack.back();
	for (size_t i = 0; i < stack.size() - 1; ++i) {
		auto& parent = stack[i];
		auto& child  = stack[i+1];
		if (child.block != current) {
			continue;
		}

		// so we already reached this point.  We need to make sure
		// that the same variables are currently tracked, so that
		// the input of each block is always well defined
		std::unordered_set<tree> tracked_vars_at_point;
		for (auto [key, _] : parent.tracked) {
			tracked_vars_at_point.insert(key);
		}

		for (auto [key, location] : last.tracked) {
			if (tracked_vars_at_point.erase(key) == 0) {
				ident name = DECL_NAME(key);

				// situation:
				// we are about to enter block current
				// last tracks key
				// parent does not track key

				// so there is a block b between
				// last->block and parent->block, which
				// untracks var.

				// we should print:
				// error at the block current (+ error msg)
				// the track in last->block
				// the untrack in b



				// TODO: backtrace
				error_at(location,
					 "Inconsistent tracking status of variable %s; only sometimes tracked at some (not this) point", IDENTIFIER_POINTER(name));
			}
		}

		for (auto var : tracked_vars_at_point) {
			ident name = DECL_NAME(var);

			// see above, but switched roles

			error_at(parent.tracked[var],
"Inconsistent tracking status of variable %s; only sometimes tracked at some (not this) point", IDENTIFIER_POINTER(name)
				);
		}

		return;
	}

	// *every* block has an info
	auto& info = infos.at(current);
	if (!info.empty()) {

		// we need to check that no newly tracked var was already tracked
		// and that every newly untracked var was already tracked
		for (auto& varloc : info.newly_tracked) {
			auto found = last.tracked.find(varloc.id);

			if (found != last.tracked.end()) {
				// TODO: error backtrace

				rich_location location{
					nullptr, varloc.loc, &NEWLY_TRACKED
				};

				location.add_range(found->second,
						   SHOW_LINES_WITHOUT_RANGE,
						   &PREVIOUSLY_TRACKED
						  );
				error_at(&location,
					 "Cannot track already tracked variable");
			}
		}

		for (auto& varloc : info.newly_untracked) {
			auto found = last.tracked.find(varloc.id);

			if (found == last.tracked.end()) {
				// error (backtrack)
				rich_location location{
					nullptr, varloc.loc, &NEWLY_UNTRACKED
				};

				error_at(&location,
					 "Cannot untrack variable that is not tracked");
			}
		}

		// new we need to merge the seen status of info into our state

		auto& state = stack.emplace_back(last);
		state.block = current;

		for (auto& [var, status] : info.seen) {
			if (status.tracked) {
				state.tracked[var] = status.last_changed;
			} else {
				state.tracked.erase(var);
			}

		}
	}

	edge e;
	edge_iterator ei;
	FOR_EACH_EDGE(e, ei, current->succs) {
		printf("--- edge %d -> %d\n",
		       current->index,
		       e->dest->index);
		violations_recurse(stack,
				   infos,
				   e->dest);
	}
}



void find_violations(const std::unordered_map<basic_block, block_info>& infos,
		     basic_block start)
{
	std::vector<state> stack;
	auto& initial = stack.emplace_back();
	initial.block = start;


	edge e;
	edge_iterator ei;
	FOR_EACH_EDGE(e, ei, start->succs) {
		printf("--- edge %d -> %d\n",
		       start->index,
		       e->dest->index);
		violations_recurse(stack,
				   infos,
				   e->dest);
	}
}

unsigned int tso::execute(function* f) {
	debug_func(f);

	auto track = maybe_get_identifier("track");
	auto untrack = maybe_get_identifier("untrack");

	if (track == NULL_TREE) {
		printf("no track defined\n");
		return 1;
	}
	if (untrack == NULL_TREE) {
		printf("no untrack defined\n");
		return 1;
	}

	context ctx;
	ctx.track = track;
	ctx.untrack = untrack;

	tree decl = f->decl;
	const std::string ident{fndecl_name(decl)};
	printf("Executing tracker for %s\n", ident.c_str());

	basic_block start = ENTRY_BLOCK_PTR_FOR_FN(f);
	basic_block end   = EXIT_BLOCK_PTR_FOR_FN(f);

	std::unordered_map<basic_block, block_info> block_infos =
		build_block_infos(start, end, ctx);

	find_violations(block_infos, start);

	return 0;
}
