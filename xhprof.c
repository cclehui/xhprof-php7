/*
 *  Copyright (c) 2009 Facebook
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef linux
/* To enable CPU_ZERO and CPU_SET, etc.     */
# define _GNU_SOURCE
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_xhprof.h"
#include "zend_extensions.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __FreeBSD__
# if __FreeBSD_version >= 700110
#   include <sys/resource.h>
#   include <sys/cpuset.h>
#   define cpu_set_t cpuset_t
#   define SET_AFFINITY(pid, size, mask) \
    cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
#   define GET_AFFINITY(pid, size, mask) \
    cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
# else
#   error "This version of FreeBSD does not support cpusets"
# endif /* __FreeBSD_version */
#elif __APPLE__
/*
 * Patch for compiling in Mac OS X Leopard
 * @author Svilen Spasov <s.spasov@gmail.com>
 */
#    include <mach/mach_init.h>
#    include <mach/thread_policy.h>
#    define cpu_set_t thread_affinity_policy_data_t
#    define CPU_SET(cpu_id, new_mask) \
    (*(new_mask)).affinity_tag = (cpu_id + 1)
#    define CPU_ZERO(new_mask)                 \
    (*(new_mask)).affinity_tag = THREAD_AFFINITY_TAG_NULL
#   define SET_AFFINITY(pid, size, mask)       \
    thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, mask, \
            THREAD_AFFINITY_POLICY_COUNT)
#else
/* For sched_getaffinity, sched_setaffinity */
# include <sched.h>
# define SET_AFFINITY(pid, size, mask) sched_setaffinity(0, size, mask)
# define GET_AFFINITY(pid, size, mask) sched_getaffinity(0, size, mask)
#endif /* __FreeBSD__ */



/**
 * **********************
 * GLOBAL MACRO CONSTANTS
 * **********************
 */

/* XHProf version                           */
#define XHPROF_VERSION       "0.9.2"

/* Fictitious function name to represent top of the call tree. The paranthesis
 * in the name is to ensure we don't conflict with user function names.  */
#define ROOT_SYMBOL                "main()"

/* Size of a temp scratch buffer            */
#define SCRATCH_BUF_LEN            512

/* Various XHPROF modes. If you are adding a new mode, register the appropriate
 * callbacks in hp_begin() */
#define XHPROF_MODE_HIERARCHICAL            1
#define XHPROF_MODE_SAMPLED            620002      /* Rockfort's zip code */

/* Hierarchical profiling flags.
 *
 * Note: Function call counts and wall (elapsed) time are always profiled.
 * The following optional flags can be used to control other aspects of
 * profiling.
 */
#define XHPROF_FLAGS_NO_BUILTINS   0x0001         /* do not profile builtins */
#define XHPROF_FLAGS_CPU           0x0002      /* gather CPU times for funcs */
#define XHPROF_FLAGS_MEMORY        0x0004   /* gather memory usage for funcs */

/* Constants for XHPROF_MODE_SAMPLED        */
#define XHPROF_SAMPLING_INTERVAL       100000      /* In microsecs        */

/* Constant for ignoring functions, transparent to hierarchical profile */
#define XHPROF_MAX_IGNORED_FUNCTIONS  256
#define XHPROF_IGNORED_FUNCTION_FILTER_SIZE                           \
    ((XHPROF_MAX_IGNORED_FUNCTIONS + 7)/8)

/* Constant for track functions */
#define XHPROF_MAX_TRACK_FUNCTIONS  256
#define XHPROF_TRACK_FUNCTION_FILTER_SIZE                           \
    ((XHPROF_MAX_TRACK_FUNCTIONS + 7)/8)

#if !defined(uint64)
typedef unsigned long long uint64;
#endif
#if !defined(uint32)
typedef unsigned int uint32;
#endif
#if !defined(uint8)
typedef unsigned char uint8;
#endif

//hp_globals.stats_count 单个元素的 hash table index
#define HP_STATS_COUNT_CT    1 
#define HP_STATS_COUNT_WT    2 
#define HP_STATS_COUNT_CPU   3 
#define HP_STATS_COUNT_MU    4 
#define HP_STATS_COUNT_PMU   5 

/**
 * *****************************
 * GLOBAL DATATYPES AND TYPEDEFS
 * *****************************
 */

/* XHProf maintains a stack of entries being profiled. The memory for the entry
 * is passed by the layer that invokes BEGIN_PROFILING(), e.g. the hp_execute()
 * function. Often, this is just C-stack memory.
 *
 * This structure is a convenient place to track start time of a particular
 * profile operation, recursion depth, and the name of the function being
 * profiled. */
typedef struct hp_entry_t {
    zend_string            *name_hprof;                       /* function name */
    int                     rlvl_hprof;        /* recursion level for function */
    uint64                  tsc_start;         /* start value for TSC counter  */
    long int                mu_start_hprof;                    /* memory usage */
    long int                pmu_start_hprof;              /* peak memory usage */
    struct rusage           ru_start_hprof;             /* user/sys time start */
    struct hp_entry_t      *prev_hprof;    /* ptr to prev entry being profiled */
    zend_long               func_hash_index;     /* func_hash_index for the function name  */
} hp_entry_t;

/* Various types for XHPROF callbacks       */
typedef void (*hp_init_cb)           (TSRMLS_D);
typedef void (*hp_exit_cb)           (TSRMLS_D);
typedef void (*hp_begin_function_cb) (hp_entry_t **entries, hp_entry_t *current   TSRMLS_DC);
typedef void (*hp_end_function_cb)   (hp_entry_t **entries  TSRMLS_DC);

/* Struct to hold the various callbacks for a single xhprof mode */
typedef struct hp_mode_cb {
    hp_init_cb             init_cb;
    hp_exit_cb             exit_cb;
    hp_begin_function_cb   begin_fn_cb;
    hp_end_function_cb     end_fn_cb;
} hp_mode_cb;

/* Xhprof's global state.
 *
 * This structure is instantiated once.  Initialize defaults for attributes in
 * hp_init_profiler_state() Cleanup/free attributes in
 * hp_clean_profiler_state() */
typedef struct hp_global_t {

    /*       ----------   Global attributes:  -----------       */

    /* Indicates if xhprof is currently enabled */
    int              enabled;

    /* Indicates if xhprof was ever enabled during this request */
    int              ever_enabled;

    /* Holds all the xhprof statistics */
    HashTable       *stats_count;

    //已捕获的函数名字 zend_string cache
    HashTable       *tracked_function_names;;

    /* Indicates the current xhprof mode or level */
    int              profiler_level;

    /* Top of the profile stack */
    hp_entry_t      *entries;

    /* freelist of hp_entry_t chunks for reuse... */
    hp_entry_t      *entry_free_list;

    /* Callbacks for various xhprof modes */
    hp_mode_cb       mode_cb;

    /*       ----------   Mode specific attributes:  -----------       */

    /* This array is used to store cpu frequencies for all available logical
     * cpus.  For now, we assume the cpu frequencies will not change for power
     * saving or other reasons. If we need to worry about that in the future, we
     * can use a periodical timer to re-calculate this arrary every once in a
     * while (for example, every 1 or 5 seconds). */
    double *cpu_frequencies;

    /* The number of logical CPUs this machine has. */
    uint32 cpu_num;

    /* The saved cpu affinity. */
    cpu_set_t prev_mask;

    /* The cpu id current process is bound to. (default 0) */
    uint32 cur_cpu_id;

    /* XHProf flags */
    uint32 xhprof_flags;

    /* Table of ignored function names and their filter */
    HashTable  *ignored_function_names;
    uint8   ignored_function_filter[XHPROF_IGNORED_FUNCTION_FILTER_SIZE];

    /* Table of function names need track , track all when null */
    HashTable  *track_function_names;
    uint8   track_function_filter[XHPROF_TRACK_FUNCTION_FILTER_SIZE];

} hp_global_t;


/**
 * ***********************
 * GLOBAL STATIC VARIABLES
 * ***********************
 */
/* XHProf global state */
static hp_global_t       hp_globals;

static ZEND_DLEXPORT void (*_zend_execute_ex)(zend_execute_data *execute_data);
static ZEND_DLEXPORT void (*_zend_execute_internal)(zend_execute_data *execute_data, zval *return_value);

ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data TSRMLS_DC);
ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data, zval *return_value);

/* Pointer to the original compile function */
static zend_op_array * (*_zend_compile_file) (zend_file_handle *file_handle, int type TSRMLS_DC);

/* Pointer to the original compile string function (used by eval) */
static zend_op_array * (*_zend_compile_string) (zval *source_string, char *filename TSRMLS_DC);

/* Bloom filter for function names to be ignored */
#define INDEX_2_BYTE(index)  (index >> 3)
#define INDEX_2_BIT(index)   (1 << (index & 0x7));


/**
 * ****************************
 * STATIC FUNCTION DECLARATIONS
 * ****************************
 */
static void hp_register_constants(INIT_FUNC_ARGS);

static void hp_begin(long level, long xhprof_flags TSRMLS_DC);
static void hp_stop(TSRMLS_D);
static void hp_end(TSRMLS_D);

static inline uint64 cycle_timer();
static double get_cpu_frequency();
static void clear_frequencies();

static void hp_free_the_free_list();
static hp_entry_t *hp_fast_alloc_hprof_entry();
static void hp_fast_free_hprof_entry(hp_entry_t *p);
static inline uint8 hp_inline_hash(zend_string *input_str);
static void get_all_cpu_frequencies();
static long get_us_interval(struct timeval *start, struct timeval *end);
static void incr_us_interval(struct timeval *start, uint64 incr);

static void hp_get_options_from_arg(HashTable *args);
static void hp_option_functions_filter_clear();
static void hp_option_functions_filter_init();

static inline zval  *hp_zval_at_key(char  *key, HashTable  *values);
static inline char **hp_strings_in_zval(zval  *values);
static inline void   hp_array_del(char **name_array);

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO(arginfo_xhprof_test, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_xhprof_enable, 0, 0, 0)
ZEND_ARG_INFO(0, flags)
ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_disable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_enable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_disable, 0)
ZEND_END_ARG_INFO()
/* }}} */

/**
 * *********************
 * FUNCTION PROTOTYPES
 * *********************
 */
int restore_cpu_affinity(cpu_set_t * prev_mask);
int bind_to_cpu(uint32 cpu_id);

/**
 * *********************
 * PHP EXTENSION GLOBALS
 * *********************
 */
/* List of functions implemented/exposed by xhprof */
zend_function_entry xhprof_functions[] = {
        PHP_FE(xhprof_test, arginfo_xhprof_test)
        PHP_FE(xhprof_enable, arginfo_xhprof_enable)
        PHP_FE(xhprof_disable, arginfo_xhprof_disable)
        {NULL, NULL, NULL}
};

/* Callback functions for the xhprof extension */
zend_module_entry xhprof_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
    STANDARD_MODULE_HEADER,
#endif
    "xhprof",                        /* Name of the extension */
    xhprof_functions,                /* List of functions exposed */
    PHP_MINIT(xhprof),               /* Module init callback */
    PHP_MSHUTDOWN(xhprof),           /* Module shutdown callback */
    PHP_RINIT(xhprof),               /* Request init callback */
    PHP_RSHUTDOWN(xhprof),           /* Request shutdown callback */
    PHP_MINFO(xhprof),               /* Module info callback */
#if ZEND_MODULE_API_NO >= 20010901
    XHPROF_VERSION,
#endif
    STANDARD_MODULE_PROPERTIES
};

PHP_INI_BEGIN()

    /* output directory:
     * Currently this is not used by the extension itself.
     * But some implementations of iXHProfRuns interface might
     * choose to save/restore XHProf profiler runs in the
     * directory specified by this ini setting.
     */
    PHP_INI_ENTRY("xhprof.output_dir", "", PHP_INI_ALL, NULL)

PHP_INI_END()

    /* Init module */
ZEND_GET_MODULE(xhprof)


/**
 * **********************************
 * PHP EXTENSION FUNCTION DEFINITIONS
 * **********************************
 */
PHP_FUNCTION(xhprof_test) {

    php_printf("xhprof_test function , zend_execute_ex pointer:%p\n", zend_execute_ex);
    php_printf("xhprof_test function , zend_execute_internal pointer:%p\n", zend_execute_internal);
}

/**
 * Start XHProf profiling in hierarchical mode.
 *
 * @param  long $flags  flags for hierarchical mode
 * @return void
 * @author kannan
 */
PHP_FUNCTION(xhprof_enable) {
    zend_long  xhprof_flags;                                    /* XHProf flags */
    HashTable *optional_array;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
                "|lh", &xhprof_flags, &optional_array) == FAILURE) {
        return;
    }

    hp_get_options_from_arg(optional_array);

    hp_begin(XHPROF_MODE_HIERARCHICAL, xhprof_flags TSRMLS_CC);
}

/**
 * Stops XHProf from profiling in hierarchical mode anymore and returns the
 * profile info.
 *
 * @param  void
 * @return array  hash-array of XHProf's profile info
 * @author kannan, hzhao
 */
PHP_FUNCTION(xhprof_disable) {
    if (hp_globals.enabled) {
        hp_stop(TSRMLS_C);
        RETURN_ARR(hp_globals.stats_count);
    }
    /* else null is returned */
}

/**
 * Module init callback.
 *
 * @author cjiang
 */
PHP_MINIT_FUNCTION(xhprof) {
    int i;

    REGISTER_INI_ENTRIES();

    hp_register_constants(INIT_FUNC_ARGS_PASSTHRU);

    /* Get the number of available logical CPUs. */
    hp_globals.cpu_num = sysconf(_SC_NPROCESSORS_CONF);

    /* Get the cpu affinity mask. */
#ifndef __APPLE__
    if (GET_AFFINITY(0, sizeof(cpu_set_t), &hp_globals.prev_mask) < 0) {
        perror("getaffinity");
        return FAILURE;
    }
#else
    CPU_ZERO(&(hp_globals.prev_mask));
#endif

    hp_globals.enabled = 0;

    /* Initialize cpu_frequencies and cur_cpu_id. */
    hp_globals.cpu_frequencies = NULL;
    hp_globals.cur_cpu_id = 0;

    hp_globals.stats_count = NULL;

    hp_globals.tracked_function_names = NULL;

    /* no free hp_entry_t structures to start with */
    hp_globals.entry_free_list = NULL;

    hp_option_functions_filter_clear();

    //function proxy

    _zend_execute_ex = zend_execute_ex;
    zend_execute_ex  = hp_execute_ex;

    _zend_execute_internal = zend_execute_internal;
    zend_execute_internal = hp_execute_internal;

#if defined(DEBUG)
    /* To make it random number generator repeatable to ease testing. */
    srand(0);
#endif
    return SUCCESS;
}

/**
 * Module shutdown callback.
 */
PHP_MSHUTDOWN_FUNCTION(xhprof) {
    /* Make sure cpu_frequencies is free'ed. */
    clear_frequencies();

    /* free any remaining items in the free list */
    hp_free_the_free_list();

    UNREGISTER_INI_ENTRIES();

    return SUCCESS;
}

/**
 * Request init callback. Nothing to do yet!
 */
PHP_RINIT_FUNCTION(xhprof) {
    return SUCCESS;
}

/**
 * Request shutdown callback. Stop profiling and return.
 */
PHP_RSHUTDOWN_FUNCTION(xhprof) {
    hp_end(TSRMLS_C);
    return SUCCESS;
}

/**
 * Module info callback. Returns the xhprof version.
 */
PHP_MINFO_FUNCTION(xhprof) {
    char buf[SCRATCH_BUF_LEN];
    char tmp[SCRATCH_BUF_LEN];
    int i;
    int len;

    php_info_print_table_start();
    php_info_print_table_header(2, "xhprof", XHPROF_VERSION);
    len = snprintf(buf, SCRATCH_BUF_LEN, "%d", hp_globals.cpu_num);
    buf[len] = 0;
    php_info_print_table_header(2, "CPU num", buf);

    if (hp_globals.cpu_frequencies) {
        /* Print available cpu frequencies here. */
        php_info_print_table_header(2, "CPU logical id", " Clock Rate (MHz) ");
        for (i = 0; i < hp_globals.cpu_num; ++i) {
            len = snprintf(buf, SCRATCH_BUF_LEN, " CPU %d ", i);
            buf[len] = 0;
            len = snprintf(tmp, SCRATCH_BUF_LEN, "%f", hp_globals.cpu_frequencies[i]);
            tmp[len] = 0;
            php_info_print_table_row(2, buf, tmp);
        }
    }

    php_info_print_table_end();
}


/**
 * ***************************************************
 * COMMON HELPER FUNCTION DEFINITIONS AND LOCAL MACROS
 * ***************************************************
 */

static void hp_register_constants(INIT_FUNC_ARGS) {
    REGISTER_LONG_CONSTANT("XHPROF_FLAGS_NO_BUILTINS",
            XHPROF_FLAGS_NO_BUILTINS,
            CONST_CS | CONST_PERSISTENT);

    REGISTER_LONG_CONSTANT("XHPROF_FLAGS_CPU",
            XHPROF_FLAGS_CPU,
            CONST_CS | CONST_PERSISTENT);

    REGISTER_LONG_CONSTANT("XHPROF_FLAGS_MEMORY",
            XHPROF_FLAGS_MEMORY,
            CONST_CS | CONST_PERSISTENT);
}

/**
 * A hash function to calculate a 8-bit hash code for a function name.
 * This is based on a small modification to 'zend_inline_hash_func' by summing
 * up all bytes of the ulong returned by 'zend_inline_hash_func'.
 *
 * @param str, char *, string to be calculated hash code for.
 *
 * @author cjiang
 */
static inline uint8 hp_inline_hash(zend_string *input_str) {
    ulong h = 5381;
    uint i = 0;
    uint8 res = 0;

    char *str = ZSTR_VAL(input_str);

    while (*str) {
        h += (h << 5);
        h ^= (ulong) *str++;
    }

    for (i = 0; i < sizeof(ulong); i++) {
        res += ((uint8 *)&h)[i];
    }
    return res;
}

/**
 * 启动配置解析， 
 * 忽略的函数 ignored_functions
 * track的函数 track_functions
 *
 * @author mpal
 */
static void hp_get_options_from_arg(HashTable *args) {

    if (hp_globals.ignored_function_names) {
        zend_hash_destroy(hp_globals.ignored_function_names);
    }

    if (hp_globals.track_function_names) {
        zend_hash_destroy(hp_globals.track_function_names);
    }

    /* Init stats_count */
    if (hp_globals.stats_count) {
        zend_hash_destroy(hp_globals.stats_count);
    }

    hp_globals.ignored_function_names = NULL;
    hp_globals.track_function_names = NULL;

    if (args != NULL) {
        zval  *z_ignored_functions = NULL;
        zval  *z_track_functions = NULL;

        //要忽略的函数
        z_ignored_functions = hp_zval_at_key("ignored_functions", args);
        if (z_ignored_functions && Z_TYPE_P(z_ignored_functions) == IS_ARRAY
                && zend_hash_num_elements(Z_ARR_P(z_ignored_functions)) > 0) {

            ALLOC_HASHTABLE(hp_globals.ignored_function_names);
            zend_hash_init(hp_globals.ignored_function_names, 20, NULL, NULL, 0);

            for (zend_hash_internal_pointer_reset(Z_ARR_P(z_ignored_functions));
                    zend_hash_has_more_elements(Z_ARR_P(z_ignored_functions)) == SUCCESS;
                    zend_hash_move_forward(Z_ARR_P(z_ignored_functions))) {

                zval *data = zend_hash_get_current_data(Z_ARR_P(z_ignored_functions));

                if (data && Z_TYPE_P(data) == IS_STRING
                        && strcmp(ZSTR_VAL(Z_STR_P(data)), ROOT_SYMBOL)) {
                    zend_hash_add_empty_element(hp_globals.ignored_function_names, Z_STR_P(data));
                }
            }
        }

        //要捕获的函数
        z_track_functions = hp_zval_at_key("track_functions", args);
        if (z_track_functions && Z_TYPE_P(z_track_functions) == IS_ARRAY
                && zend_hash_num_elements(Z_ARR_P(z_track_functions)) > 0) {

            ALLOC_HASHTABLE(hp_globals.track_function_names);
            zend_hash_init(hp_globals.track_function_names, 20, NULL, NULL, 0);

            size_t tf_count = 1;
            zval *temp_value;

            /*
            //初始化存储统计数据的hashtable
            ALLOC_HASHTABLE(hp_globals.stats_count);
            zend_hash_init(hp_globals.stats_count, 50, NULL, NULL, 0);
            zend_hash_real_init(hp_globals.stats_count, 1); //转为packed array
            */

            for (zend_hash_internal_pointer_reset(Z_ARR_P(z_track_functions));
                    zend_hash_has_more_elements(Z_ARR_P(z_track_functions)) == SUCCESS;
                    zend_hash_move_forward(Z_ARR_P(z_track_functions))) {

                zval *data = zend_hash_get_current_data(Z_ARR_P(z_track_functions));

                if (data && Z_TYPE_P(data) == IS_STRING) {
                    //zend_hash_add_empty_element(hp_globals.track_function_names, Z_STR_P(data));
                    temp_value = (zval *)emalloc(sizeof(zval));
                    ZVAL_LONG(temp_value, tf_count);

                    zend_hash_add(hp_globals.track_function_names, Z_STR_P(data), temp_value);

                    /*
                    //统计结果hashtable 初始化
                    zval *counts = (zval *)emalloc(sizeof(zval));
                    Z_TYPE_INFO_P(counts) = IS_ARRAY_EX;

                    HashTable *temp = (HashTable *)emalloc(sizeof(HashTable));
                    zend_hash_init(temp, 10, NULL, NULL, 0);
                    zend_hash_real_init(temp, 1);// 转为packed array

                    Z_ARR_P(counts) = temp;

                    zend_hash_index_add(hp_globals.stats_count, tf_count, counts);
                    //end 统计结果hashtable
                    */

                    tf_count++;
                }
            }

            //zend_hash_str_add_empty_element(hp_globals.track_function_names, ROOT_SYMBOL, strlen(ROOT_SYMBOL));

            temp_value = (zval *)emalloc(sizeof(zval));
            ZVAL_LONG(temp_value, tf_count);
            zend_hash_str_add(hp_globals.track_function_names, ROOT_SYMBOL, strlen(ROOT_SYMBOL), temp_value);

            display_hash_table(hp_globals.track_function_names);
        }
    }
}

/**
 * Clear filter for functions which may be ignored during profiling.
 *
 * @author mpal
 */
static void hp_option_functions_filter_clear() {
    memset(hp_globals.ignored_function_filter, 0,
            XHPROF_IGNORED_FUNCTION_FILTER_SIZE);

    memset(hp_globals.track_function_filter, 0,
            XHPROF_TRACK_FUNCTION_FILTER_SIZE);
}

/**
 * Initialize filter for ignored functions using bit vector.
 *
 * @author mpal
 */
static void hp_option_functions_filter_init() {

    //要忽略的函数布隆过滤器初始化
    if (hp_globals.ignored_function_names != NULL) {
        for (zend_hash_internal_pointer_reset(hp_globals.ignored_function_names);
                zend_hash_has_more_elements(hp_globals.ignored_function_names) == SUCCESS;
                zend_hash_move_forward(hp_globals.ignored_function_names)) {

            zend_string *string_index;
            zend_long long_index;

            if (HASH_KEY_IS_STRING == 
                    zend_hash_get_current_key(hp_globals.ignored_function_names, &string_index, &long_index)) {

                uint8 hash = hp_inline_hash(string_index);
                int   idx  = INDEX_2_BYTE(hash);
                hp_globals.ignored_function_filter[idx] |= INDEX_2_BIT(hash);
            }
        }
    }

    //要捕获的函数布隆过滤器初始化
    if (hp_globals.track_function_names != NULL) {
        for (zend_hash_internal_pointer_reset(hp_globals.track_function_names);
                zend_hash_has_more_elements(hp_globals.track_function_names) == SUCCESS;
                zend_hash_move_forward(hp_globals.track_function_names)) {

            zend_string *string_index;
            zend_long long_index;

            if (HASH_KEY_IS_STRING == 
                    zend_hash_get_current_key(hp_globals.track_function_names, &string_index, &long_index)) {

                uint8 hash = hp_inline_hash(string_index);
                int   idx  = INDEX_2_BYTE(hash);
                hp_globals.track_function_filter[idx] |= INDEX_2_BIT(hash);
            }
        }
    }
}

/**
 * Check if function collides in filter of functions to be ignored.
 *
 * @author mpal
 */
int hp_ignored_functions_filter_collision(uint8 hash) {
    uint8 mask = INDEX_2_BIT(hash);
    return hp_globals.ignored_function_filter[INDEX_2_BYTE(hash)] & mask;
}

int hp_track_functions_filter_collision(uint8 hash) {
    uint8 mask = INDEX_2_BIT(hash);
    return hp_globals.track_function_filter[INDEX_2_BYTE(hash)] & mask;
}

/**
 * Initialize profiler state
 *
 * @author kannan, veeve
 */
void hp_init_profiler_state(int level TSRMLS_DC) {
    /* Setup globals */
    if (!hp_globals.ever_enabled) {
        hp_globals.ever_enabled  = 1;
        hp_globals.entries = NULL;
    }
    hp_globals.profiler_level  = (int) level;

    /* Init stats_count */
    if (hp_globals.stats_count) {
        zend_hash_destroy(hp_globals.stats_count);
    }

    //初始化存储统计数据的hashtable
    ALLOC_HASHTABLE(hp_globals.stats_count);
    zend_hash_init(hp_globals.stats_count, 50, NULL, NULL, 0);
    zend_hash_real_init(hp_globals.stats_count, 1); //转为packed array

    if (hp_globals.tracked_function_names) {
        zend_hash_destroy(hp_globals.tracked_function_names);
    }
    ALLOC_HASHTABLE(hp_globals.tracked_function_names);
    zend_hash_init(hp_globals.tracked_function_names, 20, NULL, NULL, 0);

    /* NOTE(cjiang): some fields such as cpu_frequencies take relatively longer
     * to initialize, (5 milisecond per logical cpu right now), therefore we
     * calculate them lazily. */
    if (hp_globals.cpu_frequencies == NULL) {
        get_all_cpu_frequencies();
        restore_cpu_affinity(&hp_globals.prev_mask);
    }

    /* bind to a random cpu so that we can use rdtsc instruction. */
    bind_to_cpu((int) (rand() % hp_globals.cpu_num));

    /* Call current mode's init cb */
    hp_globals.mode_cb.init_cb(TSRMLS_C);

    /* 初始化要捕获和要忽略的函数的布隆过滤器 */
    hp_option_functions_filter_init();
}

/**
 * Cleanup profiler state
 *
 * @author kannan, veeve
 */
void hp_clean_profiler_state(TSRMLS_D) {
    /* Call current mode's exit cb */
    hp_globals.mode_cb.exit_cb(TSRMLS_C);

    /* Clear globals */
    if (hp_globals.stats_count) {
        zend_hash_clean(hp_globals.stats_count);
    }

    if (hp_globals.tracked_function_names) {
        zend_hash_clean(hp_globals.tracked_function_names);
    }

    hp_globals.entries = NULL;
    hp_globals.profiler_level = 1;
    hp_globals.ever_enabled = 0;

    /* Delete the array storing ignored function names */
    if (hp_globals.ignored_function_names) {
        zend_hash_clean(hp_globals.ignored_function_names);
        hp_globals.ignored_function_names = NULL;
    }

    if (hp_globals.track_function_names) {
        zend_hash_clean(hp_globals.track_function_names);
        hp_globals.track_function_names = NULL;
    }
}

/*
 * Start profiling - called just before calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define BEGIN_PROFILING(entries, symbol, func_hash_index)                  \
    do {                                                                  \
        /* Use a hash code to filter most of the string comparisons. */     \
        func_hash_index = get_func_hash_index(symbol);                 \
        if (func_hash_index) {                                                 \
            hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();              \
            (cur_entry)->func_hash_index = func_hash_index;                               \
            (cur_entry)->name_hprof = symbol;                                 \
            (cur_entry)->prev_hprof = (*(entries));                           \
            /* Call the mode's beginfn callback */                            \
            hp_globals.mode_cb.begin_fn_cb((entries), (cur_entry) TSRMLS_CC); \
            /* Update entries linked list */                                  \
            (*(entries)) = (cur_entry);                                       \
        }                                                                   \
    } while (0)

/*
 * Stop profiling - called just after calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define END_PROFILING(entries, func_hash_index)                            \
    do {                                                                  \
            hp_entry_t *cur_entry;                                            \
            /* Call the mode's endfn callback. */                             \
            /* NOTE(cjiang): we want to call this 'end_fn_cb' before */       \
            if (func_hash_index) {                                                 \
                hp_globals.mode_cb.end_fn_cb((entries) TSRMLS_CC);                \
            }                                                                   \
            cur_entry = (*(entries));                                         \
            /* Call the universal callback */                                 \
            /*hp_mode_common_endfn((entries), (cur_entry) TSRMLS_CC);*/           \
            /* Free top entry and update entries linked list */               \
            (*(entries)) = (*(entries))->prev_hprof;                          \
            hp_fast_free_hprof_entry(cur_entry);                              \
    } while (0)


/**
 * Returns formatted function name
 *
 * @param  entry        hp_entry
 * @param  result_buf   ptr to result buf
 * @param  result_len   max size of result buf
 * @return total size of the function name returned in result_buf
 * @author veeve
 */
zend_string * hp_get_entry_name(hp_entry_t  *entry) {

    //cclehui_test
    return entry->name_hprof;

    /* Add '@recurse_level' if required */
    /* NOTE:  Dont use snprintf's return val as it is compiler dependent */
    zend_string *temp_name;

    if (entry->rlvl_hprof) {
        temp_name = strpprintf(SCRATCH_BUF_LEN, "%s@%d", ZSTR_VAL(entry->name_hprof), entry->rlvl_hprof);
    } else {
        temp_name = strpprintf(SCRATCH_BUF_LEN, "%s", ZSTR_VAL(entry->name_hprof));
    }

    //zend_string_free(temp_name);

    return temp_name;
}

/**
 * Check if this entry should be ignored, first with a conservative Bloomish
 * filter then with an exact check against the function names.
 *
 * @author mpal
 */
int  hp_ignore_entry_work(uint8 hash_code, zend_string *curr_func) {
    int ignore = 0;

    if (zend_hash_exists(hp_globals.ignored_function_names, curr_func)) {
        ignore++;
    }
    /*
    if (hp_ignored_functions_filter_collision(hash_code)) {

        if (zend_hash_exists(hp_globals.ignored_function_names, curr_func)) {
            ignore++;
        }
    }
    */

    return ignore;
}

//是否是需要捕获的函数
int  hp_need_track_function(uint8 hash_code, zend_string *curr_func) {
    int track = 0;

    if (zend_hash_exists(hp_globals.track_function_names, curr_func)) {
        track++;
    }
    
    /*
    if (hp_track_functions_filter_collision(hash_code)) {

        if (zend_hash_str_exists(hp_globals.track_function_names, curr_func, strlen(curr_func))) {
            track++;
        }

        int i = 0;
        for (; hp_globals.track_function_names[i] != NULL; i++) {
            char *name = hp_globals.track_function_names[i];
            if (!strcmp(curr_func, name)) {
                track++;
                break;
            }
        }
    }
    */

    return track;
}

//是否不捕获当前函数
//获取要捕获的函数 在hash table 的index 值
static inline zend_long  get_func_hash_index(zend_string *curr_func) {
    if (hp_globals.track_function_names != NULL) {

        zval *index_value;
        index_value = zend_hash_find(hp_globals.track_function_names, curr_func);

        if (!index_value) {
            return NULL;
        }

        return Z_LVAL_P(index_value);

        //指定的函数才捕获
        /*
        if (hp_need_track_function(hash_code, curr_func)) {
            return 0;
        } else {
            return 1;
        }
        */
    }

    return NULL;

    /* check if ignoring functions is enabled */
    /*
    return hp_globals.ignored_function_names != NULL &&
        hp_ignore_entry_work(hash_code, curr_func);
        */
}

/**
 * Build a caller qualified name for a callee.
 *
 * For example, if A() is caller for B(), then it returns "A==>B".
 * Recursive invokations are denoted with @<n> where n is the recursion
 * depth.
 *
 * For example, "foo==>foo@1", and "foo@2==>foo@3" are examples of direct
 * recursion. And  "bar==>foo@1" is an example of an indirect recursive
 * call to foo (implying the foo() is on the call stack some levels
 * above).
 *
 * @author kannan, veeve
 */
size_t hp_get_function_stack(hp_entry_t *entry, int level,  zend_string **result_buf) {

    size_t len = 0;

    /* End recursion if we dont need deeper levels or we dont have any deeper
     * levels */
    if (!entry->prev_hprof || (level <= 1)) {
        *result_buf = hp_get_entry_name(entry);
        return ZSTR_LEN(*result_buf);
    }

    /* Take care of all ancestors first */
    len = hp_get_function_stack(entry->prev_hprof, level - 1, result_buf);

    /* Append the delimiter */
# define    HP_STACK_DELIM        "==>"
# define    HP_STACK_DELIM_LEN    (sizeof(HP_STACK_DELIM) - 1)

    /* Add delimiter only if entry had ancestors */
    size_t max_len = ZSTR_LEN(*result_buf) + HP_STACK_DELIM_LEN + ZSTR_LEN(entry->name_hprof);

    zend_string *temp;
    temp = strpprintf(max_len, "%s==>%s", ZSTR_VAL(*result_buf), ZSTR_VAL(entry->name_hprof));
    zend_string_free(*result_buf);

    *result_buf = temp;

# undef     HP_STACK_DELIM_LEN
# undef     HP_STACK_DELIM

    return ZSTR_LEN(*result_buf);
}

/**
 * Takes an input of the form /a/b/c/d/foo.php and returns
 * a pointer to one-level directory and basefile name
 * (d/foo.php) in the same string.
 */
static const char *hp_get_base_filename(const char *filename) {
    const char *ptr;
    int   found = 0;

    if (!filename)
        return "";

    /* reverse search for "/" and return a ptr to the next char */
    for (ptr = filename + strlen(filename) - 1; ptr >= filename; ptr--) {
        if (*ptr == '/') {
            found++;
        }
        if (found == 2) {
            return ptr + 1;
        }
    }

    /* no "/" char found, so return the whole string */
    return filename;
}

/**
 * Get the name of the current function. The name is qualified with
 * the class name if the function is in a class.
 *
 * @author kannan, hzhao
 */
static zend_string *hp_get_function_name(zend_op_array *ops TSRMLS_DC) {
    zend_execute_data *data;
    char              *ret = NULL;
    zend_function      *curr_func;

    data = EG(current_execute_data);

    if (!data) {
        return NULL;
    }

    curr_func = data->func;

    if (!curr_func->common.function_name) {
        return NULL;
    }

    //cclehui_test
    return curr_func->common.function_name;


    //没有类
    if (!curr_func->common.scope || !curr_func->common.scope->name) {
        zend_string_addref(curr_func->common.function_name);
        return curr_func->common.function_name;
    } 

    zend_string *result;

    //result = zend_string_dup(curr_func->common.scope->name, 0);
    result = zend_string_alloc(ZSTR_LEN(curr_func->common.scope->name) + ZSTR_LEN(curr_func->common.function_name) + 1 , 0);

    memcpy(result->val, ZSTR_VAL(curr_func->common.scope->name), ZSTR_LEN(curr_func->common.scope->name));
    *(result->val + ZSTR_LEN(curr_func->common.scope->name)) = ':';

    memcpy(result->val + ZSTR_LEN(curr_func->common.scope->name) + 1 , ZSTR_VAL(curr_func->common.function_name), ZSTR_LEN(curr_func->common.function_name));

    *(result->val + ZSTR_LEN(curr_func->common.scope->name) + 1 + ZSTR_LEN(curr_func->common.function_name))  = '\0';

    //cclehui_test
    //php_printf("11111111, %s, %d, %d\n", ZSTR_VAL(result), strlen(ZSTR_VAL(result)), sizeof(ZSTR_VAL(result)));

    return result;

    /*
    ret = estrdup(ZSTR_VAL(curr_func->common.function_name));
    return ret;
    */
}

/**
 * Free any items in the free list.
 */
static void hp_free_the_free_list() {
    hp_entry_t *p = hp_globals.entry_free_list;
    hp_entry_t *cur;

    while (p) {
        cur = p;
        p = p->prev_hprof;
        free(cur);
    }
}

/**
 * Fast allocate a hp_entry_t structure. Picks one from the
 * free list if available, else does an actual allocate.
 *
 * Doesn't bother initializing allocated memory.
 *
 * @author kannan
 */
static hp_entry_t *hp_fast_alloc_hprof_entry() {
    hp_entry_t *p;

    p = hp_globals.entry_free_list;

    if (p) {
        hp_globals.entry_free_list = p->prev_hprof;
        return p;
    } else {
        return (hp_entry_t *)malloc(sizeof(hp_entry_t));
    }
}

/**
 * Fast free a hp_entry_t structure. Simply returns back
 * the hp_entry_t to a free list and doesn't actually
 * perform the free.
 *
 * @author kannan
 */
static void hp_fast_free_hprof_entry(hp_entry_t *p) {

    /* we use/overload the prev_hprof field in the structure to link entries in
     * the free list. */
    p->prev_hprof = hp_globals.entry_free_list;
    hp_globals.entry_free_list = p;
}

/**
 * Increment the count of the given stat with the given count
 * If the stat was not set before, inits the stat to the given count
 *
 */
void hp_inc_count(HashTable *counts, zend_long index, long count TSRMLS_DC) {

    zval *data;

    if (!counts) return;

    data = zend_hash_index_find(counts, index); 
    if (data != NULL) {
        ZVAL_LONG(data, Z_LVAL_P(data) + count);

    } else {
        zval *temp = (zval *)emalloc(sizeof(zval));
        ZVAL_LONG(temp, count);

        zend_hash_index_add(counts, index, temp);
    }
}

/**
 * Looksup the hash table for the given symbol
 * Initializes a new array() if symbol is not present
 *
 * @author kannan, veeve
 */
HashTable * hp_stats_count_lookup(zend_long func_hash_index  TSRMLS_DC) {
    zval *counts;

    if (!hp_globals.stats_count ) {
        return NULL;
    }

    /* Lookup our hash table */
    //counts = zend_hash_find(hp_globals.stats_count, symbol); 
    counts = zend_hash_index_find(hp_globals.stats_count, func_hash_index); 
    //counts = zend_hash_index_find_cc(hp_globals.stats_count, func_hash_index); 
    if (counts == NULL) {
        /* Add symbol to hash table */
        counts = (zval *)emalloc(sizeof(zval));
        Z_TYPE_INFO_P(counts) = IS_ARRAY_EX;

        HashTable *temp = (HashTable *)emalloc(sizeof(HashTable));
        zend_hash_init(temp, 10, NULL, NULL, 0);
        //zend_hash_real_init(temp, 1);// 转为packed array

        Z_ARR_P(counts) = temp;

        php_printf("1111111111, %d, %d\n", HT_IS_PACKED(hp_globals.stats_count), HT_IS_PACKED(temp));

        zend_hash_index_add(hp_globals.stats_count, func_hash_index, counts);

        php_printf("22222222, %d, %d\n", HT_IS_PACKED(hp_globals.stats_count), HT_IS_PACKED(temp));

    }

    return Z_ARR_P(counts);
}

/**
 * Truncates the given timeval to the nearest slot begin, where
 * the slot size is determined by intr
 *
 * @param  tv       Input timeval to be truncated in place
 * @param  intr     Time interval in microsecs - slot width
 * @return void
 * @author veeve
 */
void hp_trunc_time(struct timeval *tv,
        uint64          intr) {
    uint64 time_in_micro;

    /* Convert to microsecs and trunc that first */
    time_in_micro = (tv->tv_sec * 1000000) + tv->tv_usec;
    time_in_micro /= intr;
    time_in_micro *= intr;

    /* Update tv */
    tv->tv_sec  = (time_in_micro / 1000000);
    tv->tv_usec = (time_in_micro % 1000000);
}


/**
 * ***********************
 * High precision timer related functions.
 * ***********************
 */

/**
 * Get time stamp counter (TSC) value via 'rdtsc' instruction.
 *
 * @return 64 bit unsigned integer
 * @author cjiang
 */
static inline uint64 cycle_timer() {
    uint32 __a,__d;
    uint64 val;
    asm volatile("rdtsc" : "=a" (__a), "=d" (__d));
    (val) = ((uint64)__a) | (((uint64)__d)<<32);
    return val;
}

/**
 * Bind the current process to a specified CPU. This function is to ensure that
 * the OS won't schedule the process to different processors, which would make
 * values read by rdtsc unreliable.
 *
 * @param uint32 cpu_id, the id of the logical cpu to be bound to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int bind_to_cpu(uint32 cpu_id) {
    cpu_set_t new_mask;

    CPU_ZERO(&new_mask);
    CPU_SET(cpu_id, &new_mask);

    if (SET_AFFINITY(0, sizeof(cpu_set_t), &new_mask) < 0) {
        perror("setaffinity");
        return -1;
    }

    /* record the cpu_id the process is bound to. */
    hp_globals.cur_cpu_id = cpu_id;

    return 0;
}

/**
 * Get time delta in microseconds.
 */
static long get_us_interval(struct timeval *start, struct timeval *end) {
    return (((end->tv_sec - start->tv_sec) * 1000000)
            + (end->tv_usec - start->tv_usec));
}

/**
 * Incr time with the given microseconds.
 */
static void incr_us_interval(struct timeval *start, uint64 incr) {
    incr += (start->tv_sec * 1000000 + start->tv_usec);
    start->tv_sec  = incr/1000000;
    start->tv_usec = incr%1000000;
    return;
}

/**
 * Convert from TSC counter values to equivalent microseconds.
 *
 * @param uint64 count, TSC count value
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author cjiang
 */
static inline double get_us_from_tsc(uint64 count, double cpu_frequency) {
    return count / cpu_frequency;
}

/**
 * Convert microseconds to equivalent TSC counter ticks
 *
 * @param uint64 microseconds
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author veeve
 */
static inline uint64 get_tsc_from_us(uint64 usecs, double cpu_frequency) {
    return (uint64) (usecs * cpu_frequency);
}

/**
 * This is a microbenchmark to get cpu frequency the process is running on. The
 * returned value is used to convert TSC counter values to microseconds.
 *
 * @return double.
 * @author cjiang
 */
static double get_cpu_frequency() {
    struct timeval start;
    struct timeval end;

    if (gettimeofday(&start, 0)) {
        perror("gettimeofday");
        return 0.0;
    }
    uint64 tsc_start = cycle_timer();
    /* Sleep for 5 miliseconds. Comparaing with gettimeofday's  few microseconds
     * execution time, this should be enough. */
    usleep(5000);
    if (gettimeofday(&end, 0)) {
        perror("gettimeofday");
        return 0.0;
    }
    uint64 tsc_end = cycle_timer();
    return (tsc_end - tsc_start) * 1.0 / (get_us_interval(&start, &end));
}

/**
 * Calculate frequencies for all available cpus.
 *
 * @author cjiang
 */
static void get_all_cpu_frequencies() {
    int id;
    double frequency;

    hp_globals.cpu_frequencies = malloc(sizeof(double) * hp_globals.cpu_num);
    if (hp_globals.cpu_frequencies == NULL) {
        return;
    }

    /* Iterate over all cpus found on the machine. */
    for (id = 0; id < hp_globals.cpu_num; ++id) {
        /* Only get the previous cpu affinity mask for the first call. */
        if (bind_to_cpu(id)) {
            clear_frequencies();
            return;
        }

        /* Make sure the current process gets scheduled to the target cpu. This
         * might not be necessary though. */
        usleep(0);

        frequency = get_cpu_frequency();
        if (frequency == 0.0) {
            clear_frequencies();
            return;
        }
        hp_globals.cpu_frequencies[id] = frequency;
    }
}

/**
 * Restore cpu affinity mask to a specified value. It returns 0 on success and
 * -1 on failure.
 *
 * @param cpu_set_t * prev_mask, previous cpu affinity mask to be restored to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int restore_cpu_affinity(cpu_set_t * prev_mask) {
    if (SET_AFFINITY(0, sizeof(cpu_set_t), prev_mask) < 0) {
        perror("restore setaffinity");
        return -1;
    }
    /* default value ofor cur_cpu_id is 0. */
    hp_globals.cur_cpu_id = 0;
    return 0;
}

/**
 * Reclaim the memory allocated for cpu_frequencies.
 *
 * @author cjiang
 */
static void clear_frequencies() {
    if (hp_globals.cpu_frequencies) {
        free(hp_globals.cpu_frequencies);
        hp_globals.cpu_frequencies = NULL;
    }
    restore_cpu_affinity(&hp_globals.prev_mask);
}


/**
 * ***************************
 * XHPROF DUMMY CALLBACKS
 * ***************************
 */
void hp_mode_dummy_init_cb(TSRMLS_D) { }


void hp_mode_dummy_exit_cb(TSRMLS_D) { }


void hp_mode_dummy_beginfn_cb(hp_entry_t **entries,
        hp_entry_t *current  TSRMLS_DC) { }

void hp_mode_dummy_endfn_cb(hp_entry_t **entries   TSRMLS_DC) { }


/**
 * ****************************
 * XHPROF COMMON CALLBACKS
 * ****************************
 */

/**
 * XHPROF universal end function.  This function is called for all modes after
 * the mode's specific end_function callback is called.
 *
 * @param  hp_entry_t **entries  linked list (stack) of hprof entries
 * @return void
 * @author kannan, veeve
 */
void hp_mode_common_endfn(hp_entry_t **entries, hp_entry_t *current TSRMLS_DC) {
}


/**
 * *********************************
 * XHPROF INIT MODULE CALLBACKS
 * *********************************
 */

/**
 * ************************************
 * XHPROF BEGIN FUNCTION CALLBACKS
 * ************************************
 */

/**
 * XHPROF_MODE_HIERARCHICAL's begin function callback
 *
 * @author kannan
 */
void hp_mode_hier_beginfn_cb(hp_entry_t **entries, hp_entry_t  *current  TSRMLS_DC) {

    /* Get start tsc counter */
    current->tsc_start = cycle_timer();

    /* Get CPU usage */
    if (hp_globals.xhprof_flags & XHPROF_FLAGS_CPU) {
        getrusage(RUSAGE_SELF, &(current->ru_start_hprof));
    }

    /* Get memory usage */
    if (hp_globals.xhprof_flags & XHPROF_FLAGS_MEMORY) {
        current->mu_start_hprof  = zend_memory_usage(0 TSRMLS_CC);
        current->pmu_start_hprof = zend_memory_peak_usage(0 TSRMLS_CC);
    }
}

/**
 * **********************************
 * XHPROF END FUNCTION CALLBACKS
 * **********************************
 */

/**
 * XHPROF shared end function callback
 *
 * @author kannan
 */
HashTable * hp_mode_shared_endfn_cb(hp_entry_t *top, zend_string *symbol  TSRMLS_DC) {
    HashTable    *counts;
    uint64   tsc_end;

    /* Get end tsc counter */
    tsc_end = cycle_timer();

    /* find or create the stat array */
    if (!(counts = hp_stats_count_lookup(top->func_hash_index TSRMLS_CC))) {
        return NULL;
    }

    /* Bump stats in the counts hashtable */
    hp_inc_count(counts, HP_STATS_COUNT_CT, 1  TSRMLS_CC);

    hp_inc_count(counts, HP_STATS_COUNT_WT, get_us_from_tsc(tsc_end - top->tsc_start,
                hp_globals.cpu_frequencies[hp_globals.cur_cpu_id]) TSRMLS_CC);


    return counts;
}

/**
 * XHPROF_MODE_HIERARCHICAL's end function callback
 *
 * @author kannan
 */
void hp_mode_hier_endfn_cb(hp_entry_t **entries  TSRMLS_DC) {
    hp_entry_t   *top = (*entries);
    HashTable    *counts;
    struct rusage    ru_end;
    long int         mu_end;
    long int         pmu_end;

    if (!(counts = hp_mode_shared_endfn_cb(top, top->name_hprof  TSRMLS_CC))) {
        return;
    }


    if (hp_globals.xhprof_flags & XHPROF_FLAGS_CPU) {
        /* Get CPU usage */
        getrusage(RUSAGE_SELF, &ru_end);

        /* Bump CPU stats in the counts hashtable */
        hp_inc_count(counts, HP_STATS_COUNT_CPU, (get_us_interval(&(top->ru_start_hprof.ru_utime),
                        &(ru_end.ru_utime)) +
                    get_us_interval(&(top->ru_start_hprof.ru_stime),
                        &(ru_end.ru_stime)))
                TSRMLS_CC);
    }

    if (hp_globals.xhprof_flags & XHPROF_FLAGS_MEMORY) {
        /* Get Memory usage */
        mu_end  = zend_memory_usage(0 TSRMLS_CC);
        pmu_end = zend_memory_peak_usage(0 TSRMLS_CC);

        /* Bump Memory stats in the counts hashtable */
        hp_inc_count(counts, HP_STATS_COUNT_MU,  mu_end - top->mu_start_hprof    TSRMLS_CC);
        hp_inc_count(counts, HP_STATS_COUNT_PMU, pmu_end - top->pmu_start_hprof  TSRMLS_CC);
    }
}

/**
 * ***************************
 * PHP EXECUTE/COMPILE PROXIES
 * ***************************
 */

/**
 * XHProf enable replaced the zend_execute function with this
 * new execute function. We can do whatever profiling we need to
 * before and after calling the actual zend_execute().
 *
 * @author hzhao, kannan
 */
ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data TSRMLS_DC) {
    if (!hp_globals.enabled) {
        _zend_execute_ex(execute_data TSRMLS_CC);
        return;
    }

    zend_op_array *ops = execute_data->opline;
    zend_string  *func;
    zend_long func_hash_index = 0;

    func = hp_get_function_name(ops TSRMLS_CC);

    if (!func) {
        _zend_execute_ex(execute_data TSRMLS_CC);
        return;
    }

    BEGIN_PROFILING(&hp_globals.entries, func, func_hash_index);

    _zend_execute_ex(execute_data TSRMLS_CC);

    //if (hp_globals.entries) {
    if (func_hash_index) {
        END_PROFILING(&hp_globals.entries, func_hash_index);
    }

    zend_string_release(func);
}

#undef EX
#define EX(element) ((execute_data)->element)

/**
 * Very similar to hp_execute. Proxy for zend_execute_internal().
 * Applies to zend builtin functions.
 *
 * @author hzhao, kannan
 */

ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data, zval *return_value) {

    if (!hp_globals.enabled) {
        execute_internal(execute_data, return_value);
        return;
    }

    zend_execute_data *current_data;
    zend_string *func;

    int  func_hash_index = 1;

    current_data = EG(current_execute_data);
    func = hp_get_function_name(current_data->opline TSRMLS_CC);

    if (func) {
        BEGIN_PROFILING(&hp_globals.entries, func, func_hash_index);
    }

    //执行真正的函数调用
    if (_zend_execute_internal) {
        _zend_execute_internal(execute_data, return_value);
        
    } else {
        execute_internal(execute_data, return_value);
    }

    if (func) {
        //if (hp_globals.entries) {
        if (func_hash_index) {
            END_PROFILING(&hp_globals.entries, func_hash_index);
        }
        zend_string_release(func);
    }

}


/**
 * **************************
 * MAIN XHPROF CALLBACKS
 * **************************
 */

/**
 * This function gets called once when xhprof gets enabled.
 * It replaces all the functions like zend_execute, zend_execute_internal,
 * etc that needs to be instrumented with their corresponding proxies.
 */
static void hp_begin(long level, long xhprof_flags TSRMLS_DC) {
    if (hp_globals.enabled) {
        return;
    }

    zend_long func_hash_index = 0;

    hp_globals.enabled      = 1;
    hp_globals.xhprof_flags = (uint32)xhprof_flags;

    /* Initialize with the dummy mode first Having these dummy callbacks saves
     * us from checking if any of the callbacks are NULL everywhere. */
    hp_globals.mode_cb.init_cb     = hp_mode_dummy_init_cb;
    hp_globals.mode_cb.exit_cb     = hp_mode_dummy_exit_cb;
    hp_globals.mode_cb.begin_fn_cb = hp_mode_dummy_beginfn_cb;
    hp_globals.mode_cb.end_fn_cb   = hp_mode_dummy_endfn_cb;

    /* Register the appropriate callback functions Override just a subset of
     * all the callbacks is OK. */
    switch(level) {
        case XHPROF_MODE_HIERARCHICAL:
            hp_globals.mode_cb.begin_fn_cb = hp_mode_hier_beginfn_cb;
            hp_globals.mode_cb.end_fn_cb   = hp_mode_hier_endfn_cb;
            break;
    }

    /* one time initializations */
    hp_init_profiler_state(level TSRMLS_CC);
    
    /* start profiling from fictitious main() */
    zend_string *root_symbol = zend_string_init(ROOT_SYMBOL, strlen(ROOT_SYMBOL), 1);

    BEGIN_PROFILING(&hp_globals.entries, root_symbol, func_hash_index);

}

/**
 * Called at request shutdown time. Cleans the profiler's global state.
 */
static void hp_end(TSRMLS_D) {
    /* Bail if not ever enabled */
    if (!hp_globals.ever_enabled) {
        return;
    }

    /* Stop profiler if enabled */
    if (hp_globals.enabled) {
        hp_stop(TSRMLS_C);
    }

    /* Clean up state */
    hp_clean_profiler_state(TSRMLS_C);
}

/**
 * Called from xhprof_disable(). Removes all the proxies setup by
 * hp_stop() and restores the original values.
 */
static void hp_stop(TSRMLS_D) {

    /* End any unfinished calls */
    while (hp_globals.entries) {
        END_PROFILING(&hp_globals.entries, NULL);
    }

    /* Remove proxies, restore the originals */
    zend_execute_ex       = _zend_execute_ex;
    zend_execute_internal = _zend_execute_internal;
    //zend_compile_file     = _zend_compile_file;
    //zend_compile_string   = _zend_compile_string;

    /* Resore cpu affinity. */
    restore_cpu_affinity(&hp_globals.prev_mask);

    /* Stop profiling */
    hp_globals.enabled = 0;
}


/**
 * *****************************
 * XHPROF ZVAL UTILITY FUNCTIONS
 * *****************************
 */

/** Look in the PHP assoc array to find a key and return the zval associated
 *  with it.
 *
 *  @author mpal
 **/
static zval *hp_zval_at_key(char  *key, HashTable  *values) {
    zval *result;

    size_t len = strlen(key);

    result = zend_hash_str_find(values, key, len); 

    return result;
}

/** Convert the PHP array of strings to an emalloced array of strings. Note,
 *  this method duplicates the string data in the PHP array.
 *
 *  @author mpal
 **/
static char **hp_strings_in_zval(zval  *values) {
    char   **result;
    size_t   count;
    size_t   ix = 0;

    if (!values) {
        return NULL;
    }

    if (Z_TYPE_P(values) == IS_ARRAY) {

        count = zend_hash_num_elements(Z_ARR_P(values));

        if((result =
                    (char**)emalloc(sizeof(char*) * (count + 1))) == NULL) {
            return result;
        }

        for (zend_hash_internal_pointer_reset(Z_ARR_P(values));
                zend_hash_has_more_elements(Z_ARR_P(values)) == SUCCESS;
                zend_hash_move_forward(Z_ARR_P(values))) {

            zval *data;

            /* Get the names stored in a standard array */
            if(zend_hash_get_current_key_type(Z_ARR_P(values)) == HASH_KEY_IS_LONG) {
                data = zend_hash_get_current_data(Z_ARR_P(values));

                if (!(Z_ISNULL_P(data)) &&
                        Z_TYPE_P(data) == IS_STRING &&
                        strcmp(Z_STRVAL_P(data), ROOT_SYMBOL)) { /* do not ignore "main" */
                    result[ix] = estrdup(Z_STRVAL_P(data));
                    ix++;
                }
            }
        }
    } else if(Z_TYPE_P(values) == IS_STRING) {
        if((result = (char**)emalloc(sizeof(char*) * 2)) == NULL) {
            return result;
        }
        result[0] = estrdup(Z_STRVAL_P(values));
        ix = 1;
    } else {
        result = NULL;
    }

    /* NULL terminate the array */
    if (result != NULL) {
        result[ix] = NULL;
    }

    return result;
}

/* Free this memory at the end of profiling */
static inline void hp_array_del(char **name_array) {
    if (name_array != NULL) {
        int i = 0;
        for(; name_array[i] != NULL && i < XHPROF_MAX_IGNORED_FUNCTIONS; i++) {
            efree(name_array[i]);
        }
        efree(name_array);
    }
}
