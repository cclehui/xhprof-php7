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
#include "trie.h"
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

/* Size of a temp scratch buffer            */
#define SCRATCH_BUF_LEN            512

#define XHPROF_ALGORITHM_HASH 1   // hash 查找
#define XHPROF_ALGORITHM_TRIE 2   // trie 树查找

/* profiling flags.
 *
 * Note: Function call counts and wall (elapsed) time are always profiled.
 * The following optional flags can be used to control other aspects of
 * profiling.
 */
#define XHPROF_FLAGS_NO_BUILTINS   0x0001         /* do not profile builtins */
#define XHPROF_FLAGS_CPU           0x0002      /* gather CPU times for funcs */
#define XHPROF_FLAGS_MEMORY        0x0004   /* gather memory usage for funcs */

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

#define HP_STATS_KEY_NUM   6 //统计的数据种类 比 HP_STATS_COUNT_XX定义的最大值多1

#define CLASS_FUNC_SPLIT_CHAR ':' //类名函数名连接的字符

/**
 * *****************************
 * GLOBAL DATATYPES AND TYPEDEFS
 * *****************************
 */

/* XHProf maintains a stack of entries being profiled. The memory for the entry
 * function. Often, this is just C-stack memory.
 *
 * This structure is a convenient place to track start time of a particular
 * profile operation, recursion depth, and the name of the function being
 * profiled. */
typedef struct hp_entry_t {
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
 */
typedef struct hp_global_t {

    /*       ----------   Global attributes:  -----------       */

    /* Indicates if xhprof is currently enabled */
    int              enabled;

    /* Indicates if xhprof was ever enabled during this request */
    int              ever_enabled;

    /* 抓取的结果 */
    zend_long             **stats_count;

    //当前执行的函数名 带类名
    zend_string *cur_func_name;

    /* Table of function names need track , track all when null */
    HashTable  *track_function_names; //要抓取的函数 hashtable
    hp_trie_node *track_function_trie; // 要抓取的函数， 字段树

    uint32_t track_algorithm;

    //要抓取的函数个数
    uint32_t stats_count_func_num;

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

static void hp_begin(TSRMLS_DC);
static void hp_stop(TSRMLS_D);
static void hp_end(TSRMLS_D);

static inline uint64 cycle_timer();
static double get_cpu_frequency();
static void clear_frequencies();

static void hp_free_the_free_list();
static hp_entry_t *hp_fast_alloc_hprof_entry();
static void hp_fast_free_hprof_entry(hp_entry_t *p);
static void get_all_cpu_frequencies();
static long get_us_interval(struct timeval *start, struct timeval *end);
static void incr_us_interval(struct timeval *start, uint64 incr);

static void init_options_from_arg(uint32_t track_algorithm, HashTable *args, zend_long xhprof_flags);

static inline zval  *hp_zval_at_key(char  *key, HashTable  *values);
static inline void emalloc_hp_stats_count(uint32_t func_num);
static inline void efree_hp_stats_count();
static zend_string *hp_get_function_name();

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
    zend_long  track_algorithm; //捕获使用的算法, hash查找， trie数查找
    zend_long  xhprof_flags; //捕获CPU 内存信息 配置
    HashTable *optional_array;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
                "|lhl", &track_algorithm, &optional_array, &xhprof_flags) == FAILURE) {
        return;
    }

    init_options_from_arg((uint32_t)track_algorithm, optional_array, xhprof_flags);

    hp_begin(TSRMLS_CC);
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

        int i, j = 1;
        for (i = 1; i < hp_globals.stats_count_func_num; i++) {
            for (j = 1; j < HP_STATS_KEY_NUM; j++) {
                php_printf("%d, %d => %d \n", i, j, hp_globals.stats_count[i][j]);
            }
        }

        //RETURN_ARR(hp_globals.stats_count);
    }
    /* else null is returned */
}

/**
 * Module init callback.
 *
 * @author cjiang
 */
PHP_MINIT_FUNCTION(xhprof) {

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
    hp_globals.stats_count_func_num = 0;

    /* no free hp_entry_t structures to start with */
    hp_globals.entry_free_list = NULL;

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
    REGISTER_LONG_CONSTANT("XHPROF_ALGORITHM_HASH",
            XHPROF_ALGORITHM_HASH,
            CONST_CS | CONST_PERSISTENT);

    REGISTER_LONG_CONSTANT("XHPROF_ALGORITHM_TRIE",
            XHPROF_ALGORITHM_TRIE,
            CONST_CS | CONST_PERSISTENT);

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
 * 启动配置解析， 
 * track的函数 track_functions
 *
 * @author mpal
 */
static void init_options_from_arg(uint32_t track_algorithm, HashTable *args, zend_long xhprof_flags) {

    //捕获算法
    hp_globals.track_algorithm = (track_algorithm == XHPROF_ALGORITHM_HASH ? XHPROF_ALGORITHM_HASH : XHPROF_ALGORITHM_TRIE);

    //要捕获的指标
    hp_globals.xhprof_flags = (uint32)xhprof_flags;

    //当前执行的函数名存储的内存
    if (hp_globals.cur_func_name) {
        zend_string_free(hp_globals.cur_func_name);
    }
    hp_globals.cur_func_name = zend_string_alloc(1024 , 0);

    //要捕获的函数
    if (hp_globals.track_function_names) {
        zend_hash_destroy(hp_globals.track_function_names);
    }
    hp_globals.track_function_names = NULL;

    //统计结果数据存储内存块
    if (hp_globals.stats_count) {
        efree_hp_stats_count();//释放旧内存
    }

    if (args == NULL) {
        return;
    }

    //要捕获的函数
    zval  *z_track_functions = NULL;
    z_track_functions = hp_zval_at_key("track_functions", args);
    if (!z_track_functions || Z_TYPE_P(z_track_functions) != IS_ARRAY
            || zend_hash_num_elements(Z_ARR_P(z_track_functions)) < 1) {
        return ;
    }

    ALLOC_HASHTABLE(hp_globals.track_function_names);
    zend_hash_init(hp_globals.track_function_names, 20, NULL, NULL, 0);

    size_t tf_count = 1;
    zval *temp_value;

    //emalloc hp_stats_count init 统计结果数据内存分配
    hp_globals.stats_count_func_num = zend_hash_num_elements(Z_ARR_P(z_track_functions)) + 1;
    emalloc_hp_stats_count(hp_globals.stats_count_func_num);

    //初始化字典树
    if (hp_globals.track_algorithm == XHPROF_ALGORITHM_TRIE) {
        hp_trie_init_root(&hp_globals.track_function_trie);
    }

    for (zend_hash_internal_pointer_reset(Z_ARR_P(z_track_functions));
            zend_hash_has_more_elements(Z_ARR_P(z_track_functions)) == SUCCESS;
            zend_hash_move_forward(Z_ARR_P(z_track_functions))) {

        zval *data = zend_hash_get_current_data(Z_ARR_P(z_track_functions));

        if (data && Z_TYPE_P(data) == IS_STRING) {
            temp_value = (zval *)emalloc(sizeof(zval));
            ZVAL_LONG(temp_value, tf_count);

            zend_hash_add(hp_globals.track_function_names, Z_STR_P(data), temp_value);

            //字典树
            if (hp_globals.track_algorithm == XHPROF_ALGORITHM_TRIE) {
                hp_trie_add_word(hp_globals.track_function_trie, ZSTR_VAL(Z_STR_P(data)), tf_count);
            }

            tf_count++;
        }
    }

    //cclehui_test
    //traversal(hp_globals.track_function_trie, "test");
    //display_hash_table(hp_globals.track_function_names);
    display_hash_table_new(hp_globals.track_function_names);
}

/**
 * Initialize profiler state
 *
 * @author kannan, veeve
 */
void hp_init_profiler_state() {
    /* Setup globals */
    if (!hp_globals.ever_enabled) {
        hp_globals.ever_enabled  = 1;
        hp_globals.entries = NULL;
    }

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
        efree_hp_stats_count();
    }

    hp_globals.entries = NULL;
    hp_globals.ever_enabled = 0;

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
#define BEGIN_PROFILING(entries, func_hash_index)                  \
    do {                                                                  \
        /* 判断当前函数是否需要捕获 */     \
        func_hash_index = get_func_hash_index();                 \
        if (func_hash_index) {                                                 \
            hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();              \
            (cur_entry)->func_hash_index = func_hash_index;                               \
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
            /* Free top entry and update entries linked list */               \
            (*(entries)) = (*(entries))->prev_hprof;                          \
            hp_fast_free_hprof_entry(cur_entry);                              \
    } while (0)



//是否不捕获当前函数
//获取要捕获的函数 在hash table 的index 值
static inline zend_long  get_func_hash_index() {
    if (hp_globals.track_function_names != NULL) {

        if (hp_globals.track_algorithm == XHPROF_ALGORITHM_TRIE) {

            zend_function    *cur_func;
            zend_string      *cur_class_name;
            zend_string      *cur_function_name;

            if (!EG(current_execute_data)) {
                return NULL;
            }

            cur_func = EG(current_execute_data)->func;

            if (!cur_func || !cur_func->common.function_name) {
                return NULL;
            }
            cur_function_name = cur_func->common.function_name;

            //类名
            if (cur_func->common.scope && cur_func->common.scope->name) {
                cur_class_name = cur_func->common.scope->name;
            } 

            //字典树查找
            return hp_trie_check_func(hp_globals.track_function_trie, cur_class_name, CLASS_FUNC_SPLIT_CHAR, cur_function_name);
            
        } else {
            //hash 查找
            zend_string *curr_func;
            curr_func = hp_get_function_name();

            zval *index_value;
            index_value = zend_hash_find(hp_globals.track_function_names, curr_func);

            if (!index_value) {
                return NULL;
            }

            return Z_LVAL_P(index_value);
        }

    }

    return NULL;
}

/**
 * Get the name of the current function. The name is qualified with
 * the class name if the function is in a class.
 *
 * @author kannan, hzhao
 */
static zend_string *hp_get_function_name() {
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
    //return curr_func->common.function_name;


    //没有类
    if (!curr_func->common.scope || !curr_func->common.scope->name) {
        zend_string_addref(curr_func->common.function_name);
        return curr_func->common.function_name;
    } 

    zend_string *result = hp_globals.cur_func_name;

    //result = zend_string_dup(curr_func->common.scope->name, 0);
    //result = zend_string_alloc(ZSTR_LEN(curr_func->common.scope->name) + ZSTR_LEN(curr_func->common.function_name) + 1 , 0);

    memcpy(result->val, ZSTR_VAL(curr_func->common.scope->name), ZSTR_LEN(curr_func->common.scope->name));

    *(result->val + ZSTR_LEN(curr_func->common.scope->name)) = CLASS_FUNC_SPLIT_CHAR;

    memcpy(result->val + ZSTR_LEN(curr_func->common.scope->name) + 1 , ZSTR_VAL(curr_func->common.function_name), ZSTR_LEN(curr_func->common.function_name));

    *(result->val + ZSTR_LEN(curr_func->common.scope->name) + 1 + ZSTR_LEN(curr_func->common.function_name))  = '\0';

    hp_globals.cur_func_name->len = ZSTR_LEN(curr_func->common.scope->name) + ZSTR_LEN(curr_func->common.function_name) + 1;

    return result;
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


void hp_mode_dummy_beginfn_cb(hp_entry_t **entries, hp_entry_t *current  TSRMLS_DC) { }

void hp_mode_dummy_endfn_cb(hp_entry_t **entries   TSRMLS_DC) { }


/**
 * ****************************
 * XHPROF COMMON CALLBACKS
 * ****************************
 */

/**
 * begin function callback
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
 * end function callback
 *
 * @author kannan
 */
void hp_mode_hier_endfn_cb(hp_entry_t **entries  TSRMLS_DC) {
    hp_entry_t   *top = (*entries);
    struct rusage    ru_end;
    long int         mu_end;
    long int         pmu_end;

    if (!top->func_hash_index) {
        return;
    }

    uint64   tsc_end;

    /* Get end tsc counter */
    tsc_end = cycle_timer();

    //ct 调用次数计数
    hp_globals.stats_count[top->func_hash_index][HP_STATS_COUNT_CT]++;

    //wt 函数耗时计数
    hp_globals.stats_count[top->func_hash_index][HP_STATS_COUNT_WT] += get_us_from_tsc(tsc_end - top->tsc_start,
                                            hp_globals.cpu_frequencies[hp_globals.cur_cpu_id]); 


    if (hp_globals.xhprof_flags & XHPROF_FLAGS_CPU) {
        /* Get CPU usage */
        getrusage(RUSAGE_SELF, &ru_end);

        /* Bump CPU stats in the counts hashtable */
        hp_globals.stats_count[top->func_hash_index][HP_STATS_COUNT_CPU] += (get_us_interval(&(top->ru_start_hprof.ru_utime),
                        &(ru_end.ru_utime)) +
                    get_us_interval(&(top->ru_start_hprof.ru_stime),
                        &(ru_end.ru_stime)));
    }

    if (hp_globals.xhprof_flags & XHPROF_FLAGS_MEMORY) {
        /* Get Memory usage */
        mu_end  = zend_memory_usage(0 TSRMLS_CC);
        pmu_end = zend_memory_peak_usage(0 TSRMLS_CC);

        /* Bump Memory stats in the counts hashtable */
        hp_globals.stats_count[top->func_hash_index][HP_STATS_COUNT_MU] += mu_end - top->mu_start_hprof;
        hp_globals.stats_count[top->func_hash_index][HP_STATS_COUNT_PMU] += pmu_end - top->pmu_start_hprof;
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

    zend_long func_hash_index = 0;

    BEGIN_PROFILING(&hp_globals.entries, func_hash_index);

    _zend_execute_ex(execute_data TSRMLS_CC);

    if (func_hash_index) {
        END_PROFILING(&hp_globals.entries, func_hash_index);
    }

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

    int  func_hash_index = 1;

    BEGIN_PROFILING(&hp_globals.entries, func_hash_index);

    //执行真正的函数调用
    if (_zend_execute_internal) {
        _zend_execute_internal(execute_data, return_value);
        
    } else {
        execute_internal(execute_data, return_value);
    }

    if (func_hash_index) {
        END_PROFILING(&hp_globals.entries, func_hash_index);
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
static void hp_begin(TSRMLS_DC) {
    if (hp_globals.enabled) {
        return;
    }

    zend_long func_hash_index = 0;

    hp_globals.enabled      = 1;

    /* Initialize with the dummy mode first Having these dummy callbacks saves
     * us from checking if any of the callbacks are NULL everywhere. */
    hp_globals.mode_cb.init_cb     = hp_mode_dummy_init_cb;
    hp_globals.mode_cb.exit_cb     = hp_mode_dummy_exit_cb;
    //hp_globals.mode_cb.begin_fn_cb = hp_mode_dummy_beginfn_cb;
    //hp_globals.mode_cb.end_fn_cb   = hp_mode_dummy_endfn_cb;

    hp_globals.mode_cb.begin_fn_cb = hp_mode_hier_beginfn_cb;
    hp_globals.mode_cb.end_fn_cb   = hp_mode_hier_endfn_cb;

    /* one time initializations */
    hp_init_profiler_state();
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

//malloc 并初始化内存
static void emalloc_hp_stats_count(uint32_t func_num) {
    if (func_num < 1) {
        return;
    }

    int i = 0;
    int j = 0;
    hp_globals.stats_count = (zend_long **)emalloc(sizeof(zend_long *) * func_num);
    
    for (i = 0; i < func_num; i++) {
        hp_globals.stats_count[i] = (zend_long *)emalloc(sizeof(zend_long) * HP_STATS_KEY_NUM);

        for (j = 0; j < HP_STATS_KEY_NUM; j++) {
            hp_globals.stats_count[i][j] = 0;
        }
    }

    return;
}

//回收内存
static void efree_hp_stats_count() {
    if (!hp_globals.stats_count
            || !hp_globals.stats_count_func_num) {
        return;
    }

    int i = 0;
    
    for (i = 0; i < hp_globals.stats_count_func_num; i++) {
        efree(hp_globals.stats_count[i]);
    }

    efree(hp_globals.stats_count);
}

