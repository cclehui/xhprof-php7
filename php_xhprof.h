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

#ifndef PHP_XHPROF_H
#define PHP_XHPROF_H

extern zend_module_entry xhprof_module_entry;
#define phpext_xhprof_ptr &xhprof_module_entry

#ifdef PHP_WIN32
#define PHP_XHPROF_API __declspec(dllexport)
#else
#define PHP_XHPROF_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

int callback_args(zval *val, int num_args, va_list args, zend_hash_key *hash_key) {
  zval tmp = *val;
  ZVAL_COPY(&tmp, val);
  convert_to_string(&tmp);
 
  
  php_printf("The key is : [ ");
  if (hash_key->key) {
    PHPWRITE(ZSTR_VAL(hash_key->key), ZSTR_LEN(hash_key->key));
  } else {
    php_printf("%ld", hash_key->h);
  }
  php_printf(" ], the value is: [ ");
  PHPWRITE(Z_STRVAL(tmp), Z_STRLEN(tmp));
  php_printf(" ]");
 
  zval_dtor(&tmp);
 
  return ZEND_HASH_APPLY_KEEP;
}

//打印zval
void display_zval(zval *value) {
    if (value == NULL) {
        php_printf("NULL Pointer,\n");
        return;
    }

    switch (Z_TYPE_P(value)) {
        case IS_NULL:
            /* 如果是NULL，则不输出任何东西 */
            php_printf("null, %p\n", value);
            break;

        case IS_TRUE:
            php_printf("true\n");
            break;
            
        case IS_FALSE:
            php_printf("false\n");
            break;
        case IS_LONG:
            /* 如果是long整型，则输出数字形式 */
            php_printf("%ld\n", Z_LVAL_P(value));
            break;
        case IS_DOUBLE:
            /* 如果是double型，则输出浮点数 */
            php_printf("%f\n", Z_DVAL_P(value));
            break;
        case IS_STRING:
            /* 如果是string型，则二进制安全的输出这个字符串 */
            //PHPWRITE(Z_STRVAL_P(value), Z_STRLEN_P(value));
            php_printf("%s\n", Z_STRVAL_P(value));
            break;
        case IS_RESOURCE:
            php_printf("Resource #%ld\n", Z_RESVAL_P(value));
            break;
        case IS_ARRAY:
            /* 如果是Array，则输出Array5个字母！ */
            php_printf("Array\n");
            break;
        case IS_OBJECT:
            php_printf("Object\n");
            break;
        default:
             php_printf("Unknown\n");
             break;
    }
}

static int php_sample_print_zval(zval *val TSRMLS_DC) {
    // 重新copy一个zval，防止破坏原数据
    zval tmpcopy ;
    ZVAL_COPY(&tmpcopy, val) ;  

    // 转换为字符串
    //INIT_PZVAL(&tmpcopy);
    convert_to_string(&tmpcopy);

    // 开始输出
    php_printf("php_sample_print_zval, value:%s\n", Z_STRVAL(tmpcopy));

    //返回，继续遍历下一个
    return ZEND_HASH_APPLY_KEEP;
}

static int php_hash_print_with_args(zval *val, int num_args, va_list args, zend_hash_key *hash_key) {
    // 重新copy一个zval，防止破坏原数据
    zval tmpcopy ;
    ZVAL_COPY(&tmpcopy, val) ;  

    // 转换为字符串
    //INIT_PZVAL(&tmpcopy);
    convert_to_string(&tmpcopy);

    // 开始输出
    if (hash_key->key) {
        php_printf("php_sample_print_zval, %s => %s\n", ZSTR_VAL(hash_key->key), Z_STRVAL(tmpcopy));
        
    } else {
        php_printf("php_sample_print_zval, null => %s\n", Z_STRVAL(tmpcopy));
    }

    //返回，继续遍历下一个
    return ZEND_HASH_APPLY_KEEP;
}

//打印hashtable
void display_hash_table(HashTable *ht) {
    //zend_hash_apply(ht, php_sample_print_zval TSRMLS_CC);
    if (ht == NULL) {
        php_printf("display_hash_table: ht is Null\n");
    }

    php_printf("display_hash_table: ht nNumOfElements: %d\n", ht->nNumOfElements);

    zend_hash_apply_with_arguments(ht, php_hash_print_with_args, 0);
}

static zend_always_inline Bucket *zend_hash_find_bucket_cc(const HashTable *ht, zend_string *key) {
    zend_ulong h;
    uint32_t nIndex;
    uint32_t idx;
    Bucket *p, *arData;

    h = zend_string_hash_val(key);
    arData = ht->arData;
    nIndex = h | ht->nTableMask;
    idx = HT_HASH_EX(arData, nIndex);
    while (EXPECTED(idx != HT_INVALID_IDX)) {
        p = HT_HASH_TO_BUCKET_EX(arData, idx);
        if (EXPECTED(p->key == key)) { /* check for the same interned string */
            return p;
        } else if (EXPECTED(p->h == h) &&
                EXPECTED(p->key) &&
                EXPECTED(ZSTR_LEN(p->key) == ZSTR_LEN(key)) &&
                EXPECTED(memcmp(ZSTR_VAL(p->key), ZSTR_VAL(key), ZSTR_LEN(key)) == 0)) {
            return p;
        }
        idx = Z_NEXT(p->val);
    }
    return NULL;
}


PHP_MINIT_FUNCTION(xhprof);
PHP_MSHUTDOWN_FUNCTION(xhprof);
PHP_RINIT_FUNCTION(xhprof);
PHP_RSHUTDOWN_FUNCTION(xhprof);
PHP_MINFO_FUNCTION(xhprof);

PHP_FUNCTION(xhprof_test);
PHP_FUNCTION(xhprof_enable);
PHP_FUNCTION(xhprof_disable);
PHP_FUNCTION(xhprof_sample_enable);
PHP_FUNCTION(xhprof_sample_disable);

#endif /* PHP_XHPROF_H */
