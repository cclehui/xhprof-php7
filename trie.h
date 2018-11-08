
#ifndef PHP_XHPROF_TRIE
#define PHP_XHPROF_TRIE

#include <stdio.h>
#include <stdlib.h>

#define TRUE 1
#define FALSE 0

//#define SUB_NODE_COUNT 26
//#define SUB_NODE_COUNT 94
#define SUB_NODE_COUNT 127
#define START_ASCII '!'

typedef struct node {
    struct node* children[SUB_NODE_COUNT];
    int flag; // true 代表一个单词的结束  false 还有子节点
    char character;
    zend_long func_hash_index;
} hp_trie_node;


hp_trie_node* hp_create_node(char c, int flag) {
    hp_trie_node* n = emalloc(sizeof(hp_trie_node));
    n->character = c;
    n->flag = flag;
    n->func_hash_index = NULL;

    int i;
    for (i = 0; i < SUB_NODE_COUNT; i++) {
        n->children[i] = NULL;
    }
    return n;
}

hp_trie_node* hp_create_root() {
    hp_trie_node *root = hp_create_node('$', FALSE);
    return root;
}

//创建和初始化trie树
void hp_trie_init_root(hp_trie_node **root_ptr) {
    if (*root_ptr) {
        hp_efree_trie(*root_ptr);
    }

    (*root_ptr) = hp_create_root();
}

int append_node(hp_trie_node* n, char c) {

    //hp_trie_node* child_ptr =  n->children[c - START_ASCII];
    hp_trie_node* child_ptr =  n->children[c];

    if (child_ptr) {
        return FALSE;

    } else {
        //n->children[c - START_ASCII] = hp_create_node(c, FALSE);
        n->children[c] = hp_create_node(c, FALSE);
        return TRUE;
    }
}

int hp_trie_add_word(hp_trie_node* root, char* str, zend_long func_hash_index) {
    char c = *str;
    hp_trie_node* ptr = root;
    int flag = TRUE;
    while(c != '\0') {
        if (!append_node(ptr, c)) {
            flag = FALSE;
        }
        //ptr = ptr->children[c - START_ASCII];
        ptr = ptr->children[c];
        c = *(++str);
    }

    if (!ptr->flag) {
        flag = FALSE;
        ptr->flag = TRUE;
        ptr->func_hash_index = func_hash_index;
    }
    return !flag;
}

//释放trie数内存
void hp_efree_trie(hp_trie_node* root) {
    if (!root) {
        return;
    }

    if (root->flag) {
        efree(root);
        return;
    }

    int i;
    for (i = 0; i < SUB_NODE_COUNT; i++) {
        hp_efree_trie(root->children[i]);
    }
}

void traversal(hp_trie_node* root, char* str) {

    if (!root) {
        return;
    }

    int len_of_str = strlen(str);
    char* new_str = malloc(len_of_str+1);
    strcpy(new_str, str);
    new_str[len_of_str] = root->character;

    if (root->flag) {
        //输出
        char* str_for_print = malloc(len_of_str+2);
        strcpy(str_for_print, new_str);
        str_for_print[len_of_str+1] = '\0';
        php_printf("ttttttttttt\t%s\n", str_for_print);
        free(str_for_print);
    }

    int i;

    for (i = 0; i < SUB_NODE_COUNT; i++) {
        traversal(root->children[i], new_str);
    }
    free(new_str);
}

int hp_trie_check_func(hp_trie_node* root, zend_string *class_name, char split_char, zend_string *function_name ) {

    hp_trie_node* ptr = root;
    int i;

    if (class_name != NULL) {
        for (i = 0; i < ZSTR_LEN(class_name); i++) {
            if (!ptr) {
                return FALSE;
            }
            ptr = ptr->children[class_name->val[i]];
        }

        for (i = 0; i < 1; i++) {
            if (!ptr) {
                return FALSE;
            }
            ptr = ptr->children[split_char];
        }
    }

    for (i = 0; i < ZSTR_LEN(function_name); i++) {
        if (!ptr) {
            return FALSE;
        }
        ptr = ptr->children[function_name->val[i]];
    }

    if (ptr && ptr->flag) {
        return ptr->func_hash_index;

    } else {
        return FALSE;
    }
}

int hp_trie_check(hp_trie_node* root, char* word, int len) {
    hp_trie_node* ptr = root;
    if (len == NULL) {
        len = strlen(word);
    }

    int i;
    for (i = 0; i < len; i++) {
        if (!ptr) {
            return FALSE;
        }
        //ptr = ptr->children[word[i] - START_ASCII];
        ptr = ptr->children[word[i]];
    }
    if (ptr && ptr->flag) {
        return ptr->func_hash_index;
        //return TRUE;

    } else {
        return FALSE;
    }
}

#undef TRUE 
#undef FALSE
#undef SUB_NODE_COUNT
#undef START_ASCII

#endif
