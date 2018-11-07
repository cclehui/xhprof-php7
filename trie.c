
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
    int flag;
    char character;
} hp_trie_node;

hp_trie_node* create_node(char c, int flag) {
    hp_trie_node* n = emalloc(sizeof(hp_trie_node));
    n->character = c;
    n->flag = flag;
    for (int i = 0; i < SUB_NODE_COUNT; i++) {
        n->children[i] = NULL;
    }
    return n;
}

hp_trie_node* create_root() {
    hp_trie_node *root = create_node('$', FALSE);
    return root;
}

int append_node(hp_trie_node* n, char c) {

    //hp_trie_node* child_ptr =  n->children[c - START_ASCII];
    hp_trie_node* child_ptr =  n->children[c];

    if (child_ptr) {
        return FALSE;

    } else {
        //n->children[c - START_ASCII] = create_node(c, FALSE);
        n->children[c] = create_node(c, FALSE);
        return TRUE;
    }
}

int add_word(hp_trie_node* root, char* str) {
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
    }
    return !flag;
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

    for (int i = 0; i < SUB_NODE_COUNT; i++) {
        traversal(root->children[i], new_str);
    }
    free(new_str);
}

int check(hp_trie_node* root, char* word) {
    hp_trie_node* ptr = root;
    int len = strlen(word);
    for (int i = 0; i < len; i++) {
        if (!ptr) {
            return FALSE;
        }
        //ptr = ptr->children[word[i] - START_ASCII];
        ptr = ptr->children[word[i]];
    }
    if (ptr && ptr->flag) {
        return TRUE;

    } else {
        return FALSE;
    }
}

#undef TRUE 
#undef FALSE
#undef SUB_NODE_COUNT
#undef START_ASCII

#endif
