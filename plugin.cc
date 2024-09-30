#include <string>
#include <string_view>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cassert>
#include <span>

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <c-tree.h>

#include <context.h>
#include <tree.h>
#include <tree-pass.h>
#include <print-tree.h>
#include <line-map.h>
#include <langhooks.h>

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

// use ident if we know that its an IDENTIFIER_NODE
using ident = tree;

ident track_id = NULL_TREE;
ident untrack_id = NULL_TREE;

struct my_rich_location : public rich_location {
	explicit my_rich_location (location_t loc, const range_label *label = NULL)
		: rich_location(line_table, loc, label)
		{}
};

void define_builtins(void*, void*)
{
	// this seems to work, sadly we cannot make untrack return its
	//
	track_id = get_identifier("__plugin_track_variable");
	untrack_id = get_identifier("__plugin_untrack_variable");
	// tree type = build_varargs_function_type_list(void_type_node, NULL_TREE);

	// tree track = build_decl(UNKNOWN_LOCATION, FUNCTION_DECL, track_id, type);
	// tree untrack = build_decl(UNKNOWN_LOCATION, FUNCTION_DECL, untrack_id, type);

	// TREE_PUBLIC(track) = 1;
	// DECL_EXTERNAL(track) = 1;

	// pushdecl(track);

	// TREE_PUBLIC(untrack) = 1;
	// DECL_EXTERNAL(untrack) = 1;

	// pushdecl(untrack);
}

void define_features(void*, void*)
{
	bool feature = true;
	// not sure what the real way to do this but we somehow need to
	// call the static function `init_has_feature()` before registering
	// features.  As the function is static this is not possible directly,
	// but we can take advantage of the fact that `has_feature_p` has to
        // initialize the features before checking for availability.  This
	// is why we call it here.  I guess we should maybe throw an error
	// if this feature already exists somehow ??

	if (!has_feature_p("variable_tracking", feature)) {
		c_common_register_feature("variable_tracking", feature);
	}
}

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

	register_callback(plugin_info->base_name, PLUGIN_START_UNIT,
			  define_builtins, NULL);

	register_callback(plugin_info->base_name, PLUGIN_START_UNIT,
			  define_features, NULL);


// // Macros provided for convenience.
// #ifdef __has_feature
// #if __has_feature(address_sanitizer)
// #define ASAN_DEFINE_REGION_MACROS
// #endif
// #elif defined(__SANITIZE_ADDRESS__)
// #define ASAN_DEFINE_REGION_MACROS
// #endif

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

	std::cout << std::string(n, '-') << " " << top_file << ":" << file <<
			":" << ident << std::endl;
}

static void
debug_stmt(/* const */ gimple* st)
{
	enum gimple_code code = gimple_code(st);

	std::cout << "- " << code;

	std::cout << " | ";
	debug_gimple_stmt(st);
}

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
simple_text_label UNTRACKED{"not tracked here"};
simple_text_label TRACKED{"tracked here"};
simple_text_label PREVIOUSLY_UNTRACKED{"previously untracked here"};
simple_text_label NEWLY_UNTRACKED{"newly untracked here"};
simple_text_label PREVIOUSLY_DECLARED{"variable declared here"};
simple_text_label RETURNED_FROM{"returned from here"};

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
			my_rich_location location{loc, &NEWLY_TRACKED};

			location.add_range(status.last_changed,
					   SHOW_LINES_WITHOUT_RANGE,
					   &PREVIOUSLY_TRACKED);


			error_at(loc, // &location
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
			my_rich_location location{loc, &NEWLY_UNTRACKED};

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

//	tree block = TREE_BLOCK(arg);

//	printf("location = %d\n", EXPR_LOCATION(block));

	// error_at(EXPR_LOCATION(arg), "Oh oh");

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

void handle_gimple(block_info& current, gimple_stmt_iterator si, const context& ctx)
{
	gimple* st = gsi_stmt(si);

	debug_stmt(st);

	if (is_gimple_call(st)) {
		// search for track/untrack

		tree fdecl = gimple_call_fndecl(st); // TREE_CODE() = FUNCTION_DECL

		ident name = DECL_NAME(fdecl);

		printf("name = %s\n", IDENTIFIER_POINTER(name));
		if (name == ctx.track) {
			printf("found track\n");
			start_tracking(current, st);
			gsi_replace(&si, gimple_build_nop(), true);
		} else if (name == ctx.untrack) {
			printf("found untrack\n");
			stop_tracking(current, st);
			gsi_replace(&si, gimple_build_nop(), true);
		}
		// } else if (name == ctx.replace) {
		// 	unsigned num_args = gimple_call_num_args(st);
		// 	if (num_args == 1) {
		// 		tree lhs = gimple_call_lhs(st);
		// 		gimple_stmt_iterator gsi = gsi_for_stmt(st);
		// 		gimple_set_no_warning(st, true);
		// 		if (lhs == NULL_TREE) {
		// 			gsi_remove(&gsi, true);
		// 		} else {
		// 			tree rhs = gimple_call_arg(st, 0);
		// 			gassign* assign = gimple_build_assign(lhs, rhs);
		// 			gsi_replace(&gsi, assign, true);
		// 		}
		// 		//update_stmt_if_modified(st);
		// 	}
		// }



		// tree fn = gimple_call_fn(st); // TREE_CODE() = ADDR_EXPR ??

		// if (num_args == 1) {
		// 	warning_at(loc, OPT_Wunused_result,
		// 		   "found %qD",
		// 		   fdecl
		// 		  );
		// 	//printf("One found\n");
		// }
	}

	if (gimple_clobber_p(st)) {
		// a clobber is always an assign, think of
		// var = CLOBBER;
		// sadly this is never hit ...
		tree var = gimple_assign_lhs(st);

		printf(" CLOBBER %s \n", IDENTIFIER_POINTER(var));

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


		printf("---  start block %d --- \n", current->index);
		for (gimple_stmt_iterator si = gsi_start_bb(current);
		     !gsi_end_p(si); gsi_next(&si)) {
			handle_gimple(info, si, ctx);
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

std::string print_stack(std::span<const state> stack)
{
	std::string result = "[";

	for (size_t i = 0; i < stack.size(); ++i) {
		if (i != 0) {
			result += ", ";
		} else {
			result += " ";
		}
		result += std::to_string(stack[i].block->index);
	}

	if (stack.size() != 0) { result += " "; }
	result += "]";
	return result;
}


std::pair<location_t, location_t> block_bounds(basic_block b)
{
	location_t start{UNKNOWN_LOCATION}, end{UNKNOWN_LOCATION};

	for (gimple_stmt_iterator si = gsi_start_bb(b);
	     !gsi_end_p(si); gsi_next(&si)) {
		gimple* stmt = gsi_stmt(si);

		location_t loc = gimple_location(stmt);

		debug_stmt(stmt);
		printf("  -> loc = %u\n", loc);

		if (loc == UNKNOWN_LOCATION) {
			continue;
		}

		if (start == UNKNOWN_LOCATION) {
			start = loc;
		}

		end = loc;
	}
	// for (block_stmt_iterator si = bsi_start(b);
	//      !bsi_end_p(si); bsi_next(&si)) {
	// }
	return { start, end };
}

location_t block_end(basic_block b)
{
	auto [_, end] = block_bounds(b);
	return end;
}

location_t block_begin(basic_block b)
{
	auto [begin, _] = block_bounds(b);
	return begin;
}

location_t block_start(basic_block b)
{
	for (gimple_stmt_iterator si = gsi_start_bb(b);
	     !gsi_end_p(si); gsi_next(&si)) {
		gimple* stmt = gsi_stmt(si);

		location_t loc = gimple_location(stmt);
		if (loc != UNKNOWN_LOCATION) {
			return loc;
		}
	}
	return UNKNOWN_LOCATION;
}

location_t last_track_pos(std::span<const state> stack,
			    tree var,
			    const std::unordered_map<basic_block, block_info>& infos)
{
	for (size_t i = stack.size(); i > 0;) {

		i -= 1;

		auto& state = stack[i];
		basic_block block = state.block;
		auto& info = infos.at(block);

		auto found = info.seen.find(var);

		if (found == info.seen.end()) { continue; }

		auto& status = found->second;

		if (!status.tracked) {

			tree name = DECL_NAME(var);
			fprintf(stderr, "bad var %s: traceback %s\n", IDENTIFIER_POINTER(name), print_stack(stack).c_str());


			assert(!"supposed untracked variable is somehow last seen as tracked");
		}


		// so this variable was last seen in this block
		// and it is untracked, so we know that the last time it was

		return status.last_changed;


	}

	// this should not happen

	return UNKNOWN_LOCATION;
}

location_t last_untrack_pos(std::span<const state> stack,
			    tree var,
			    const std::unordered_map<basic_block, block_info>& infos)
{
	for (size_t i = stack.size(); i > 0;) {

		i -= 1;

		auto& state = stack[i];
		basic_block block = state.block;
		auto& info = infos.at(block);

		auto found = info.seen.find(var);

		if (found == info.seen.end()) { continue; }

		auto& status = found->second;

		if (status.tracked) {

			tree name = DECL_NAME(var);
			fprintf(stderr, "bad var %s: traceback %s\n", IDENTIFIER_POINTER(name), print_stack(stack).c_str());


			assert(!"supposed untracked variable is somehow last seen as tracked");
		}


		// so this variable was last seen in this block
		// and it is untracked, so we know that the last time it was

		return status.last_changed;

	}

	// this should not happen

	return UNKNOWN_LOCATION;
}

void inconsistent_tracking_error(
	basic_block b,
	tree var,
	location_t tracked_loc,
	location_t untracked_loc)
{
	auto start = block_start(b);
	ident name = DECL_NAME(var);

	my_rich_location loc{ start, nullptr };

	loc.add_range(tracked_loc,
		      SHOW_LINES_WITHOUT_RANGE,
		      &PREVIOUSLY_TRACKED);

	loc.add_range(untracked_loc,
		      SHOW_LINES_WITHOUT_RANGE,
		      &PREVIOUSLY_UNTRACKED);

	loc.add_range(DECL_SOURCE_LOCATION(var),
		      SHOW_LINES_WITHOUT_RANGE,
		      &PREVIOUSLY_DECLARED);

	error_at(&loc,
		 "Inconsistent tracking status of variable %s; only sometimes tracked at some (not this) point",
		 IDENTIFIER_POINTER(name));
}

void violations_recurse(std::vector<state> stack,
			const std::unordered_map<basic_block, block_info>& infos,
                        // we come enter 'current' from 'from'
			basic_block from,
			basic_block current,
			basic_block end)
{
	assert(stack.size() > 0);
	auto& last = stack.back();

	// check for reentry into the same block
	printf("checking for reentry into %p (%d)\n", current, current->index);
	printf("  stack = %s\n", print_stack(stack).c_str());
	for (size_t i = 0; i < stack.size() - 1; ++i) {
		auto& parent = stack[i];
		auto& child  = stack[i+1];

		printf(" child = %p (%d)\n", child.block, child.block->index);
		printf(" parent = %p (%d)\n", parent.block, parent.block->index);

		if (child.block != current) {
			printf("  -> not same (child != current)\n");


			continue;
		}

		printf("reached %p (%d) again (child == current)\n",
		       current, current->index);

		// so we already reached this point.  We need to make sure
		// that the same variables are currently tracked, so that
		// the input of each block is always well defined
		std::unordered_set<tree> tracked_vars_at_point;
		for (auto [key, _] : parent.tracked) {
			tracked_vars_at_point.insert(key);
		}

		for (auto [key, location] : last.tracked) {
			if (tracked_vars_at_point.erase(key) == 0) {
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

				my_rich_location loc{ block_begin(current), nullptr };
				// loc.add_range(last_track_pos(std::span(stack.begin() + i, // parent loc
				// 				       stack.end()),
				// 			     key, infos),
				// 	      SHOW_LINES_WITHOUT_RANGE,
				// 	      &NEWLY_TRACKED);
				loc.add_range(block_end(parent.block),
					      SHOW_LINES_WITHOUT_RANGE,
					      &UNTRACKED);
				loc.add_range(block_end(last.block),
					      SHOW_LINES_WITHOUT_RANGE,
					      &TRACKED);

				error_at(&loc,
					 "inconsistent tracking status of %qD at point",
					 key);

				// does not seem to work when adding this location
				// as range, so report it seperately
				error_at(last_track_pos(std::span(stack.begin() + i, // parent loc
								       stack.end()),
							     key, infos), "inconsistency caused by this track");

				return;
			}
		}

		for (auto var : tracked_vars_at_point) {
			location_t untrack_pos = last_untrack_pos(stack,
								  var,
								  infos);

			inconsistent_tracking_error(current,
						    var,
						    parent.tracked[var],
						    untrack_pos);

			printf("inconsistent tracking error\n");

			return;
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
				my_rich_location location{
					varloc.loc, &NEWLY_TRACKED
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
				printf("untracking not tracked\n");
				printf("  stack = %s\n", print_stack(stack).c_str());
				printf("  current = %d\n", current->index);


				// error (backtrack)
				my_rich_location location{varloc.loc, &NEWLY_UNTRACKED};

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
	} else {
		auto& state = stack.emplace_back(last);
		state.block = current;
	}

	// if reached end, make sure that nothing is still tracked
	if (current == end) {
		// TODO: instead of checking this just at the end block,
		//       we should check this for each declaration when it
		//       goes out of scope

		// 0: int f() {
		// 1: 	{
		// 2: 		int x;
		// 3: 		track(x);
		// 4: 	}
		// 5: 	return 3;
		// 6: }

		// currently we report the error at 5, when we should do so
		// at 4

		// apperently gcc inserts "CLOBBERS" at the end of the lifetime
		// This does not always happen though.

		// for this we need to find out which declaration belongs to
		// which basic_block.
		// This could be done with a tree -> basic_block function
		// if such a thing exists

		// printf("reached end with %zu tracked vars\n", last.tracked.size());
		// return;

		for (auto [var, tracked_loc] : last.tracked) {

			my_rich_location loc{
				tracked_loc, &TRACKED
			};

			loc.add_range(DECL_SOURCE_LOCATION(var),
				      SHOW_LINES_WITHOUT_RANGE,
				      &PREVIOUSLY_DECLARED);

			// maybe we should use the goto_locus for
			// the edge here

			location_t endloc = block_end(from);

			loc.add_range(endloc,
				      SHOW_LINES_WITHOUT_RANGE,
				      &RETURNED_FROM);

			error_at(&loc,
				 "did not untrack %qD before returning (last = %d) %u",
				 var, from->index, endloc);
		}
	} else {
		edge e;
		edge_iterator ei;
		auto current_size = stack.size();
		FOR_EACH_EDGE(e, ei, current->succs) {
			printf("--- edge %d -> %d\n",
			       current->index,
			       e->dest->index);


			// reset stack for new path
			stack.resize(current_size);
			violations_recurse(stack,
					   infos,
					   current,
					   e->dest,
					   end);
		}
	}
}

void find_violations(const std::unordered_map<basic_block, block_info>& infos,
		     basic_block start, basic_block end)
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
		stack.resize(1);
		violations_recurse(stack, infos,
				   start, e->dest, end);
	}
}

unsigned int tso::execute(function* f) {
	debug_func(f);

	// auto track = maybe_get_identifier("track");
	// auto untrack = maybe_get_identifier("untrack");

	// if (track == NULL_TREE) {
	// 	printf("no track defined\n");
	// 	return 1;
	// }
	// if (untrack == NULL_TREE) {
	// 	printf("no untrack defined\n");
	// 	return 1;
	// }

	context ctx;
	ctx.track = track_id;
	ctx.untrack = untrack_id;

	tree decl = f->decl;
	const std::string ident{fndecl_name(decl)};
	printf("Executing tracker for %s\n", ident.c_str());

	basic_block start = ENTRY_BLOCK_PTR_FOR_FN(f);
	basic_block end   = EXIT_BLOCK_PTR_FOR_FN(f);

	std::unordered_map<basic_block, block_info> block_infos =
		build_block_infos(start, end, ctx);

	find_violations(block_infos, start, end);

	printf("finished finding violations in %s...\n", ident.c_str());

	return 0;
}
