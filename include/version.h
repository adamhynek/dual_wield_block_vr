#ifndef	DWBVR_VERSION_INCLUDED
#define DWBVR_VERSION_INCLUDED

#define MAKE_STR_HELPER(a_str) #a_str
#define MAKE_STR(a_str) MAKE_STR_HELPER(a_str)

#define DWBVR_VERSION_MAJOR	1
#define DWBVR_VERSION_MINOR	6
#define DWBVR_VERSION_PATCH	0
#define DWBVR_VERSION_BETA	0
#define DWBVR_VERSION_VERSTRING	MAKE_STR(DWBVR_VERSION_MAJOR) "." MAKE_STR(DWBVR_VERSION_MINOR) "." MAKE_STR(DWBVR_VERSION_PATCH) "." MAKE_STR(DWBVR_VERSION_BETA)

#endif
