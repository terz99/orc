#ifndef RESTCONF_UCI_GET_H
#define RESTCONF_UCI_GET_H

#include "cgi.h"
#include "error.h"
#include "uci/methods.h"

int add_to_json(struct json_object *check, struct json_object *top_level,
                char *key, const char *type, error *err);
char **uci_get_children_references(struct UciPath *path, error *err);
struct json_object *uci_get_list(struct json_object *yang, struct UciPath *path,
                                 error *err);
struct json_object *uci_get_leaf(struct json_object *yang, struct UciPath *path,
                                 error *err);
struct json_object *uci_get_leaf_list(struct json_object *yang,
                                      struct UciPath *path, error *err);

#endif  // RESTCONF_UCI_GET_H
