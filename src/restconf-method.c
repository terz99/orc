#include <ctype.h>

#include "restconf-method.h"
#include "error.h"
#include "http.h"
#include "restconf-json.h"
#include "restconf-verify.h"
#include "uci/cmd.h"
#include "uci/uci-get.h"
#include "uci/uci-util.h"
#include "url.h"
#include "util.h"
#include "vector.h"
#include "yang-util.h"
#include "yang-verify.h"

#define GET 0
#define POST 1
#define PUT 2
#define DELETE 3

int is_null_or_empty(char *str) {
  return (str == NULL || strlen(str) == 0);
}

static int requires_keys(int request) {
  if (request == POST) {
    return 0;
  }
  return 1;
}

static UciWritePair **verify_content_yang_util(struct json_object *content,
                                               struct json_object *yang_node,
                                               struct UciPath *path, error *err,
                                               int root, int check_exists) {
  UciWritePair **command_list = NULL;
  const char *child_type = NULL;
  if (!(child_type = json_get_string(yang_node, YANG_TYPE))) {
    *err = YANG_SCHEMA_ERROR;
    return NULL;
  }
  if (yang_is_leaf_list(child_type)) {
    return restconf_verify_leaf_list(content, yang_node, command_list, err,
                                     check_exists, path);
  } else if (yang_is_leaf(child_type)) {
    if ((*err = restconf_verify_leaf(content, path, yang_node, check_exists)) !=
        RE_OK) {
      return NULL;
    }
    UciWritePair *output = initialize_uci_write_pair(
        path, (char *)json_object_get_string(content), option);
    if (!output) {
      *err = INTERNAL;
      return NULL;
    }
    vector_push_back(command_list, output);
    return command_list;
  } else if (yang_is_list(child_type) && path->where == 0) {
    json_type value_type = json_object_get_type(content);
    if (value_type != json_type_array && value_type != json_type_object) {
      *err = INVALID_TYPE;
      return NULL;
    }
    int list_length = uci_list_length(path);
    if (root) {
      if (value_type != json_type_object) {
        *err = MULTIPLE_OBJECTS;
        return NULL;
      }
      struct json_object *existing_values = NULL;
      if (check_exists) {
        existing_values = uci_get_list(yang_node, path, err);
        if (*err != RE_OK) {
          return NULL;
        }
      }
      if (!existing_values) {
        existing_values = json_object_new_array();
      }
      // combine with uci values when root is list
      if (value_type == json_type_array) {
        json_array_forloop(content, index) {
          struct json_object *item = json_object_array_get_idx(content, index);
          if (!item) {
            *err = INTERNAL;
            return NULL;
          }
          json_object_array_add(existing_values, item);
        }
      } else {
        json_object_array_add(existing_values, content);
      }
      *err = json_yang_verify_list(existing_values, yang_node);
      if (*err != RE_OK) {
        return NULL;
      }
    } else {
      struct json_object *array = json_object_new_array();
      if (value_type == json_type_array) {
        json_array_forloop(content, idx) {
          json_object_array_add(array, json_object_array_get_idx(content, idx));
        }
      } else if (value_type == json_type_object) {
        json_object_array_add(array, content);
      } else {
        *err = INTERNAL;
        return NULL;
      }

      *err = json_yang_verify_list(array, yang_node);

      if (*err != RE_OK) {
        return NULL;
      }
    }
    if (value_type == json_type_array) {
      int start = (path->where || !root) ? path->index : list_length;
      for (int index = 0, pos = start;
           index < json_object_array_length(content); index++, pos++) {
        struct json_object *tmp = json_object_array_get_idx(content, index);
        path->index = pos;
        path->where = 1;
        struct UciPath *path_dup = uci_dup_path(path);
        if (!path_dup) {
          return NULL;
        }
        UciWritePair **tmp_list =
            verify_content_yang_util(tmp, yang_node, path_dup, err, 0, check_exists);
        if (*err != RE_OK) {
          free_uci_write_list(tmp_list);
          return NULL;
        }

        for (size_t i = 0; i < vector_size(tmp_list); i++) {
          vector_push_back(command_list, tmp_list[i]);
        }
        vector_free(tmp_list);
        free(path_dup);
      }
    } else {
      path->index = (path->where || !root) ? path->index : list_length;
      path->where = 1;
      UciWritePair **tmp_list =
          verify_content_yang_util(content, yang_node, path, err, 0, check_exists);
      if (*err != RE_OK) {
        free_uci_write_list(tmp_list);
        return NULL;
      }
      for (size_t i = 0; i < vector_size(tmp_list); i++) {
        vector_push_back(command_list, tmp_list[i]);
      }
      vector_free(tmp_list);
    }
    path->index = 0;
    path->where = 0;
    return command_list;
  }
  if (yang_is_list(child_type)) {
    get_leaf_as_name(yang_node, content, path);
  }
  json_object_object_foreach(content, key, val) {
    struct json_object *child = NULL;
    char *module = NULL;
    char *split_key = NULL;
    if (split_pair_by_char(key, &module, &split_key, ':')) {
      split_key = key;
    }
    if (module) {
      free(module);
    }
    child = json_get_object_from_map(yang_node, split_key);
    free(split_key);
    if (!child) {
      *err = NO_SUCH_ELEMENT;
      return NULL;
    }

    const char *child_t = NULL;
    if (!(child_t = json_get_string(child, YANG_TYPE))) {
      *err = YANG_SCHEMA_ERROR;
      return NULL;
    }

    if (yang_is_list(child_t) && get_leaf_as_type(child, path)) {

      struct UciPath *path_dup = uci_dup_path(path);
      if (!path_dup) {
        return NULL;
      }
      UciWritePair **tmp_list = restconf_verify_nested_lists(val, child, path_dup, err);
      if (*err != RE_OK) {
        free_uci_write_list(tmp_list);
        return NULL;
      }

      for (size_t i = 0; i < vector_size(tmp_list); i++) {
        vector_push_back(command_list, tmp_list[i]);
      }
      vector_free(tmp_list);
    }

    get_path_from_yang(child, path);
    struct UciWritePair **tmp_list = verify_content_yang_util(val, child, path, err, 0, check_exists);
    if (*err != RE_OK) {
      free_uci_write_list(tmp_list);
      return NULL;
    }
    for (size_t i = 0; i < vector_size(tmp_list); i++) {
      vector_push_back(command_list, tmp_list[i]);
    }
    vector_free(tmp_list);
  }
  *err = RE_OK;
  return command_list;
}

void verify_section_names(UciWritePair **cmds, error *err) {
  *err = RE_OK;
  for (size_t i = 0; i < vector_size(cmds); i++) {
    UciWritePair *cmd = cmds[i];
    if (is_null_or_empty(cmd->path.section_type) || is_null_or_empty(cmd->path.section)) {
      continue;
    }
    for (int j = 0; j < strlen(cmd->path.section); j++) {
      if (!(isalnum(cmd->path.section[j]) || cmd->path.section[j] == '_')) {
        *err = INVALID_TYPE;
        return;
      }
    }
  }
}

static UciWritePair **verify_content_yang(struct json_object *content,
                                               struct json_object *yang_node,
                                               struct UciPath *path, error *err,
                                               int root, int check_exists) {
  UciWritePair **cmds = verify_content_yang_util(content, yang_node, path, err, root, check_exists);
  if (*err != RE_OK) {
    return NULL;
  }

  verify_section_names(cmds, err);
  return cmds;
}

static error get_list_item_where(struct json_object *yang, char *keylist,
                                 struct UciPath *uci) {
  struct json_object *keys = NULL;
  int array_length;

  if (!(keys = json_get_array(yang, YANG_KEYS))) {
    return YANG_SCHEMA_ERROR;
  }
  array_length = json_object_array_length(keys);
  rvec keylist_vec = clist_to_vec(keylist);
  if (!keylist_vec || array_length != vector_size(keylist_vec)) {
    // not as many keys as keys specified
    return LIST_UNDEFINED_KEY;
  }
  map_str2str key_value[array_length];
  for (int index = 0; index < array_length; index++) {
    struct json_object *key = NULL;
    struct json_object *key_child = NULL;
    const char *uci_option_name = NULL;
    const char *yang_option_name = NULL;

    key = json_object_array_get_idx(keys, index);
    if (!key || json_object_get_type(key) != json_type_string) {
      return YANG_SCHEMA_ERROR;
    }
    yang_option_name = json_object_get_string(key);
    key_child = json_get_object_from_map(yang, yang_option_name);
    if (!key_child) {
      return LEAF_NO_OPTION;
    }
    uci_option_name = json_get_string(key_child, YANG_UCI_OPTION);
    if (uci_option_name == NULL) {
      return LEAF_NO_OPTION;
    }
    key_value[index].key = (char *)uci_option_name;
    key_value[index].str = keylist_vec[index];
  }
  struct UciWhere where = {
      .path = uci, .key_value = key_value, .key_value_length = array_length};
  int where_index = uci_index_where(&where);
  if (where_index == -1) {
    uci->index = uci_list_length(uci);
    uci->where = 1;
    return LIST_UNDEFINED_KEY;
  }

  uci->where = 1;
  uci->index = where_index;
  vector_free(keylist_vec);
  return RE_OK;
}



static error check_path(struct json_object **root_yang, char **path,
    size_t start, size_t end, struct UciPath *uci, int check_keys,
    int stop_at_key, int request) {
  struct json_object *iter = *root_yang;
  struct json_object *keys = NULL;
  size_t i;
  if (!path) {
    return INTERNAL;
  }
  for (i = start; i < end; i++) {
    char *path_computed = NULL;
    char *obj = NULL;
    char *keylist = NULL;
    char *keylist_encoded = NULL;
    const char *type = NULL;
    struct json_object *child = NULL;
    error err;

    split_pair_by_char(path[i], &obj, &keylist_encoded, '=');
    if (obj && keylist_encoded) {
      keylist = malloc(strlen(keylist_encoded) + 1);
      urldecode(keylist, keylist_encoded);
      free(keylist_encoded);
    }

    if (!(path_computed = obj)) {
      path_computed = path[i];
    }

    if (check_keys && keys && json_value_in_array(keys, path_computed)) {
      return DELETING_KEY;
    }
    keys = NULL;

    if (!(child = json_get_object_from_map(iter, path_computed))) {
      return NO_SUCH_ELEMENT;
    }

    get_path_from_yang(child, uci);
    if (requires_keys(request) && keylist && uci->parent
      && get_leaf_as_type(child, uci->parent)) {
      char **ref_names = uci_get_children_references(uci->parent, &err);
      uci->parent->option = "";

      char target_name[512];
      int size = snprintf(target_name, sizeof(target_name), "%s_%s", uci->parent->section, keylist);
      target_name[size] = '\0';
      for (size_t j = 0; j < vector_size(ref_names); j++) {
        if (strcmp(ref_names[j], target_name) == 0) {
          uci->section = ref_names[j];
          break;
        }
      }

      if (is_null_or_empty(uci->section)) {
        return NO_SUCH_ELEMENT;
      }
    }

    type = json_get_string(child, YANG_TYPE);
    if (type && yang_is_list(type)) {
      if (requires_keys(request) && !keylist) {
        return LIST_NO_FILTER;
      }

      int next_is_end = i + 1 == end;
      if (!next_is_end) {
        return LIST_NO_FILTER;
      }

      if (requires_keys(request) && is_null_or_empty(uci->section)
        && (err = get_list_item_where(child, keylist, uci)) != RE_OK) {
        if ((err != LIST_UNDEFINED_KEY && !stop_at_key) || !stop_at_key) {
          return err;
        }
      }

      if (check_keys && !(keys = json_get_array(child, YANG_KEYS))) {
        return YANG_SCHEMA_ERROR;
      }
    } else if (type && yang_is_leaf_list(type)) {
      int next_is_end = i + 1 == end;
      if (!next_is_end) {
        return LIST_NO_FILTER;
      }
    }

    if (keylist) {
      free(keylist);
    }
    if (obj) {
      free(obj);
    }
    iter = child;
  }
  *root_yang = iter;
  return RE_OK;
}

struct json_object *build_recursive(struct json_object *jobj,
                                    struct UciPath *path, error *err,
                                    char *ref_name, int root) {
  struct json_object *map = NULL;
  char path_string[512];
  const char *type = NULL;
  const char *type_string = NULL;
  if (!(type = json_get_string(jobj, YANG_TYPE))) {
    *err = YANG_SCHEMA_ERROR;
    return NULL;
  }
  if (!root) {
    get_path_from_yang(jobj, path);
    if (ref_name != NULL) {
      path->section = ref_name;
    }
  }

  if (yang_is_leaf(type)) {
    return uci_get_leaf(jobj, path, err);
  } else if (yang_is_leaf_list(type)) {
    struct json_object *retval = uci_get_leaf_list(jobj, path, err);
    path->option = "";
    return retval;
  } else if (yang_is_list(type)) {
    return uci_get_list(jobj, path, err);
  }

  uci_combine_to_path(path, path_string, sizeof(path_string));
  if ((strlen(path->section) != 0 || strlen(path->option) != 0) &&
      !uci_path_exists(path_string)) {
    *err = NO_SUCH_ELEMENT;
    return NULL;
  }

  struct json_object *top_level = json_object_new_object();
  json_object_object_get_ex(jobj, YANG_MAP, &map);
  json_object_object_foreach(map, key, val) {
    error err_rec = RE_OK;
    if (!(type_string = json_get_string(val, YANG_TYPE))) {
      *err = YANG_SCHEMA_ERROR;
      return NULL;
    }
    if (yang_is_container(type_string) || yang_is_list(type_string)) {
      continue;
    }
    struct json_object *check = build_recursive(val, path, &err_rec, NULL, 0);
    if (!add_to_json(check, top_level, key, NULL, &err_rec)) {
      *err = err_rec;
      return NULL;
    }
  }

  json_object_object_foreach(map, object_key, value) {
    error err_rec = RE_OK;
    if (!(type_string = json_get_string(value, YANG_TYPE))) {
      *err = YANG_SCHEMA_ERROR;
      return NULL;
    }
    if (yang_is_leaf(type_string) || yang_is_leaf_list(type_string)) {
      continue;
    }

    if (get_leaf_as_type(value, path)) {
      char **ref_names = uci_get_children_references(path, err);
      for (size_t i = 0; i < vector_size(ref_names); i++) {
        struct json_object *check = build_recursive(value, path, &err_rec, ref_names[i], 0);
        if (!add_to_json(check, top_level, object_key, type, &err_rec)) {
          *err = err_rec;
          return NULL;
        }
      }
    } else {
      struct json_object *check = build_recursive(value, path, &err_rec, NULL, 0);
      if (!add_to_json(check, top_level, object_key, type, &err_rec)) {
        *err = err_rec;
        return NULL;
      }
    }
  }

  *err = RE_OK;
  if (json_object_object_length(top_level) == 0) {
    return NULL;
  }
  return top_level;
}

int data_get(struct CgiContext *cgi, char **pathvec) {
  json_object *module = NULL;
  json_object *top_level = NULL;
  json_object *yang_tree = NULL;
  char *module_name = NULL;
  char *top_level_name = NULL;
  const char *type_string = NULL;
  int retval = 1;
  error err;

  if (split_pair_by_char(pathvec[1], &module_name, &top_level_name, ':')) {
    retval = restconf_badrequest();
    goto done;
  }

  struct UciPath uci = INIT_UCI_PATH();

  module = yang_module_exists(module_name);
  if (!module) {
    retval = restconf_unknown_namespace();
    goto done;
  }
  get_path_from_yang(module, &uci);
  top_level = json_get_object_from_map(module, top_level_name);
  if (!top_level) {
    retval = restconf_badrequest();
    goto done;
  }
  get_path_from_yang(top_level, &uci);
  err = check_path(&top_level, pathvec, 2, vector_size(pathvec), &uci, 0, 0, GET);
  if (!top_level || err != RE_OK) {
    retval = print_error(err);
    goto done;
  }
  type_string = json_get_string(top_level, YANG_TYPE);
  if (!type_string) {
    retval = restconf_badrequest();
    goto done;
  }
  err = RE_OK;
  yang_tree = build_recursive(top_level, &uci, &err, NULL, 1);
  if (!yang_tree && err != RE_OK) {
    retval = print_error(err);
    goto done;
  } else if (!yang_tree && err == RE_OK) {
    yang_tree = json_object_new_object();
  }
  content_type_json();
  headers_end();
  if (yang_is_leaf(type_string) || yang_is_leaf_list(type_string) ||
      yang_is_list(type_string)) {
    struct json_object *parent = json_object_new_object();
    char buf[512];
    char *object_requested = pathvec[vector_size(pathvec) - 1];
    char *equal_delimiter = strchr(object_requested, '=');
    int equal_delimiter_index = (size_t)(equal_delimiter - object_requested);
    retval = snprintf(buf, sizeof(buf), "%s:%.*s", module_name,
                      equal_delimiter_index, object_requested);
    if (retval < 0) {
      retval = internal_server_error(cgi);
      goto done;
    }
    json_object_object_add(parent, buf, yang_tree);
    json_pretty_print(parent);
    json_object_put(parent);
  } else if (yang_is_container(type_string)) {
    struct json_object *parent = json_object_new_object();
    char path_string[512];
    size_t size = 0;
    for (size_t i = 1; i < vector_size(pathvec); i++) {
      strcat(path_string, pathvec[i]);
      strcat(path_string, ":");
      size = size + strlen(pathvec[i]) + 1;
    }
    path_string[size-1] = '\0';
    json_object_object_add(parent, path_string, yang_tree);
    json_pretty_print(parent);
    json_object_put(parent);
  }
  retval = 0;
done:
  if (module_name) {
    free(module_name);
  }
  if (top_level_name) {
    free(top_level_name);
  }
  if (yang_tree) {
    json_object_put(yang_tree);
  }
  return retval;
}

int data_post(struct CgiContext *cgi, char **pathvec, int root) {
  json_object *module = NULL;
  json_object *top_level = NULL;
  struct json_object *content = NULL;
  char *module_name = NULL;
  char *top_level_name = NULL;
  char *content_raw = NULL;
  char *root_key_copy = NULL;
  char *root_key = NULL;
  struct json_object *root_object = NULL;
  const char *type_string = NULL;
  int retval = 1;
  int exists;
  error err;
  enum json_tokener_error parse_error;
  char path_string[512];
  char key_out[1024];
  UciWritePair *container_create = NULL;
  UciWritePair *reference_create = NULL;
  UciWritePair **cmds = NULL;
  struct UciPath uci = INIT_UCI_PATH();

  if ((content_raw = get_content()) == NULL) {
    retval = restconf_malformed();
    goto done;
  }
  content = json_tokener_parse_verbose(content_raw, &parse_error);
  if (parse_error != json_tokener_success) {
    retval = restconf_malformed();
    goto done;
  }
  if (json_object_object_length(content) != 1) {
    // Only 1 child is allowed
    retval = restconf_malformed();
    goto done;
  }

  json_object_object_foreach(content, root_key_string, root_val) {
    root_key = root_key_string;
    root_object = root_val;
  }

  if (root) {
    if (split_pair_by_char(root_key, &module_name, &top_level_name, ':')) {
      retval = restconf_badrequest();
      goto done;
    }
  } else {
    if (split_pair_by_char(pathvec[1], &module_name, &top_level_name, ':')) {
      retval = restconf_badrequest();
      goto done;
    }
  }

  if (!(module = yang_module_exists(module_name))) {
    retval = restconf_unknown_namespace();
    goto done;
  }

  get_path_from_yang(module, &uci);
  top_level = json_get_object_from_map(module, top_level_name);
  if (!top_level) {
    retval = restconf_badrequest();
    goto done;
  }
  get_path_from_yang(top_level, &uci);

  if (root) {
    content = root_object;
    get_path_from_yang(content, &uci);
  } else {
    err = check_path(&top_level, pathvec, 2, vector_size(pathvec), &uci, 1, 0, POST);
    content = root_object;
    if (!top_level || err != RE_OK) {
      retval = print_error(err);
      goto done;
    }
    if (uci.parent && get_leaf_as_type(top_level, uci.parent) && is_null_or_empty(uci.section)) {
      struct json_object *key = NULL;
      json_object_object_get_ex(top_level, YANG_KEYS, &key);
      if (json_object_get_type(key) == json_type_array && json_object_array_length(key) > 1) {
        retval = print_error(MULTIPLE_OBJECTS);
        goto done;
      }
      struct json_object *key_object = json_object_array_get_idx(key, 0);
      if (json_object_get_type(key_object) != json_type_string) {
        retval = print_error(INVALID_TYPE);
        goto done;
      }
      if (is_null_or_empty(uci.section_type)) {
        retval = print_error(YANG_SCHEMA_ERROR);
        goto done;
      }
      const char *key_string = json_object_get_string(key_object);
      char *key_value = (char *) json_get_string(content, key_string);
      if (!key_value) {
        retval = print_error(KEY_NOT_PRESENT);
        goto done;
      }

      uci.section = key_value;
      char section_name[512];
      int size = snprintf(section_name, sizeof(section_name), "%s_%s",
          uci.parent->section, uci.section);
      section_name[size] = '\0';

      reference_create = initialize_uci_write_pair(uci.parent, section_name, list);
      if (!reference_create) {
        retval = restconf_operation_failed_internal();
        goto done;
      }
      uci.parent->option = "";
    }
  }

  type_string = json_get_string(top_level, YANG_TYPE);
  if (!type_string || yang_is_leaf(type_string)) {
    restconf_badrequest();
    goto done;
  }

  uci_combine_to_path(&uci, path_string, sizeof(path_string));
  exists = uci_path_exists(path_string);
  if (exists) {
    retval = restconf_data_exists();
    goto done;
  }
  if (json_object_get_type(content) != json_type_object) {
    retval = restconf_malformed();
    goto done;
  }

  if (root) {
    container_create = initialize_uci_write_pair(&uci, NULL, container);
    if (!container_create) {
      retval = restconf_operation_failed_internal();
      goto done;
    }
  }
  root_key_copy = str_dup(root_key);
  struct json_object *created = NULL;
  if ((created = json_get_object_from_map(top_level, root_key_copy))) {
    if (yang_is_list(json_get_string(created, YANG_TYPE))) {
      struct json_object *keys = NULL;
      if ((keys = json_get_array(created, YANG_KEYS))) {
        struct json_object *values = NULL;
        if (json_extract_key_values(keys, root_object, &values) == RE_OK) {
          json_object_object_foreach(values, key, value) {
            strcat(key_out, json_object_get_string(value));
            strcat(key_out, ",");
          }
          key_out[strlen(key_out) - 1] = '\0';
        }
      }
    }
  }
  cmds = verify_content_yang(content, top_level, &uci, &err, 0, 1);
  if (err != RE_OK) {
    retval = print_error(err);
    goto done;
  }
  if (container_create) {
    vector_push_back(cmds, container_create);
  }
  if (reference_create) {
    vector_push_back(cmds, reference_create);
  }
  write_uci_write_list(cmds);
  printf("Status: 201 Created\r\n");
  char *protocol = NULL;
  char *slash = "";
  char *equal = "";
  if (strlen(key_out) > 0) {
    equal = "=";
  }
  if (cgi->https) {
    protocol = "https://";
  } else {
    protocol = "http://";
  }
  if (cgi->path_full[strlen(cgi->path_full) - 1] != '/') {
    slash = "/";
  }
  printf("Location: %s%s%s%s%s%s%s\r\n", protocol, cgi->host, cgi->path_full,
         slash, root_key_copy, equal, key_out);
  headers_end();
done:
  if (module_name) {
    free(module_name);
  }
  if (top_level_name) {
    free(top_level_name);
  }
  if (cmds) {
    free_uci_write_list(cmds);
  }
  if (root_key_copy) {
    free(root_key_copy);
  }
  return retval;
}

int data_put(struct CgiContext *cgi, char **pathvec, int root) {
  // Cannot update the key values for list item
  char *content_raw = NULL;
  char *module_name = NULL;
  char *top_level_name = NULL;
  char *root_key = NULL;
  struct json_object *root_object = NULL;
  struct json_object *content = NULL;
  struct json_object *module = NULL;
  json_object *top_level = NULL;
  struct UciPath uci = INIT_UCI_PATH();
  struct UciPath delete_uci = INIT_UCI_PATH();
  enum json_tokener_error parse_error;
  UciWritePair **cmds = NULL;
  char **package_list = NULL;
  struct UciPath *delete = NULL;
  char key_out[1024];
  error err;
  int retval = 1;

  if ((content_raw = get_content()) == NULL) {
    retval = restconf_malformed();
    goto done;
  }
  content = json_tokener_parse_verbose(content_raw, &parse_error);
  if (parse_error != json_tokener_success) {
    retval = restconf_malformed();
    goto done;
  }
  if (json_object_object_length(content) != 1) {
    // Only 1 child is allowed
    retval = restconf_malformed();
    goto done;
  }

  json_object_object_foreach(content, root_key_string, root_val) {
    root_key = root_key_string;
    root_object = root_val;
  }

  if (root) {
    if (split_pair_by_char(root_key, &module_name, &top_level_name, ':')) {
      retval = restconf_badrequest();
      goto done;
    }
  } else {
    if (split_pair_by_char(pathvec[1], &module_name, &top_level_name, ':')) {
      retval = restconf_badrequest();
      goto done;
    }
  }

  if (!(module = yang_module_exists(module_name))) {
    retval = restconf_unknown_namespace();
    goto done;
  }
  get_path_from_yang(module, &uci);

  top_level = json_get_object_from_map(module, top_level_name);
  if (!top_level) {
    retval = restconf_badrequest();
    goto done;
  }
  get_path_from_yang(top_level, &uci);

  if ((vector_size(pathvec) == 2 || root)) {
    content = root_object;
    get_path_from_yang(content, &uci);
  } else {
    err = check_path(&top_level, pathvec, 2, vector_size(pathvec), &uci, 1, 1, PUT);
    if (!top_level || err != RE_OK) {
      retval = print_error(err);
      goto done;
    }
  }

  if (json_object_get_type(content) != json_type_object) {
    retval = restconf_malformed();
    goto done;
  }

  free(module_name);
  free(top_level_name);
  module_name = NULL;
  top_level_name = NULL;
  split_pair_by_char(root_key, &module_name, &top_level_name, ':');
  if (!top_level_name) {
    top_level_name = root_key;
  }
  char *item = NULL;
  char *keys = NULL;
  char sep = '=';
  if (vector_size(pathvec) == 2 || root) {
    sep = ':';
  }
  split_pair_by_char(pathvec[vector_size(pathvec) - 1], &item, &keys, sep);
  if (!item) {
    item = pathvec[vector_size(pathvec) - 1];
  }
  if ((vector_size(pathvec) == 2 || root) &&
      strcmp(top_level_name, keys) != 0) {
    retval = restconf_badrequest();
    goto done;
  } else if (!(vector_size(pathvec) == 2 || root) &&
             strcmp(top_level_name, item) != 0) {
    retval = restconf_badrequest();
    goto done;
  }

  if (item) free(item);
  if (keys) free(keys);

  delete_uci = uci;

  if (yang_is_list(json_get_string(top_level, YANG_TYPE))) {
    struct json_object *keys = NULL;
    if ((keys = json_get_array(top_level, YANG_KEYS))) {
      struct json_object *values = NULL;
      if (json_extract_key_values(keys, root_object, &values) == RE_OK) {
        key_out[0] = '\0';
        json_object_object_foreach(values, key, value) {
          strcat(key_out, json_object_get_string(value));
          strcat(key_out, ",");
        }
        key_out[strlen(key_out) - 1] = '\0';
      }
    }
  }

  cmds = verify_content_yang(root_object, top_level, &uci, &err,
                                  !(vector_size(pathvec) == 2 || root), 0);
  if (err != RE_OK) {
    retval = print_error(err);
    goto done;
  }

  if (strlen(key_out) > 0) {
    char *obj = NULL;
    char *keylist_encoded = NULL;
    char *keylist = NULL;
    split_pair_by_char(pathvec[vector_size(pathvec) - 1], &obj,
                       &keylist_encoded, '=');
    if (obj && keylist_encoded) {
      keylist = malloc(strlen(keylist_encoded) + 1);
      urldecode(keylist, keylist_encoded);
      free(keylist_encoded);
    }
    if (strcmp(keylist, key_out) != 0) {
      free(obj);
      free(keylist);
      retval = restconf_malformed();
      goto done;
    }
    free(obj);
    free(keylist);
  }

  delete = extract_paths(top_level, &delete_uci, &err);
  if (err != RE_OK) {
    retval = print_error(err);
    goto done;
  }

  int created_or_updated = -1;

  for (size_t i = 0; i < vector_size(delete); i++) {
    char path_string[512];
    struct UciPath tmp = delete[i];

    uci_combine_to_path(&tmp, path_string, sizeof(path_string));

    if (!is_in_vector(package_list, tmp.package)) {
      vector_push_back(package_list, tmp.package);
    }

    if ((created_or_updated = uci_delete_path(path_string, false)) == 1) {
      retval = print_error(INTERNAL);
      uci_revert_all(package_list);
      goto done;
    }
  }

  if (uci_commit_all(package_list)) {
    retval = restconf_partial_operation();
    goto done;
  }

  write_uci_write_list(cmds);

  if (created_or_updated == -1) {
    printf("Status: 201 Created\r\n");
    headers_end();
  } else {
    printf("Status: 204 No Content\r\n");
    headers_end();
  }
  retval = 0;
done:
  if (module_name) {
    free(module_name);
  }
  if (top_level_name) {
    free(top_level_name);
  }
  if (cmds) {
    free_uci_write_list(cmds);
  }
  return retval;
}

static struct UciPath *extract_nested_list_paths(struct UciPath *path_list,
    struct json_object *node, struct UciPath *uci, error *err) {
  struct UciPath *res = NULL;
  struct UciPath *nested_path_list = extract_paths(node, uci, err);
  if (*err != RE_OK) {
    return NULL;
  }
  for (size_t i = 0; i < vector_size(path_list); i++) {
    vector_push_back(res, path_list[i]);
  }
  for (size_t i = 0; i < vector_size(nested_path_list); i++) {
    vector_push_back(res, nested_path_list[i]);
  }
  return res;
}

static struct UciPath *extract_list_paths(struct UciPath *path_list, struct json_object *node, struct UciPath *uci, error *err) {
  vector_push_back(path_list, *uci);
  json_object_object_foreach(node, key, val) {
    const char *subnode_type = NULL;
    if (!(subnode_type = json_get_string(val, YANG_TYPE))) {
      *err = YANG_SCHEMA_ERROR;
      return NULL;
    }
    if (yang_is_leaf(subnode_type) || yang_is_leaf_list(subnode_type)) {
      continue;
    }
    get_path_from_yang(val, uci);
    if (uci->parent && get_leaf_as_type(val, uci->parent)) {
      char **ref_names = uci_get_children_references(uci->parent, err);
      for (size_t i = 0; i < vector_size(ref_names); i++) {
        uci->section = ref_names[i];
        path_list = extract_nested_list_paths(path_list, val, uci, err);
      }
    } else {
      path_list = extract_nested_list_paths(path_list, val, uci, err);
    }
  }
  *err = RE_OK;
  return path_list;
}

struct UciPath *extract_paths(struct json_object *node, struct UciPath *uci,
                              error *err) {
  struct json_object *map = NULL;
  struct UciPath *path_list = NULL;

  const char *type = NULL;
  if (!(type = json_get_string(node, YANG_TYPE))) {
    *err = YANG_SCHEMA_ERROR;
    return NULL;
  }

  if (yang_is_leaf(type)) {
    // add path of leaf
    vector_push_back(path_list, *uci);
    uci->option = "";
    *err = RE_OK;
    return path_list;
  } else if (yang_is_leaf_list(type)) {
    // add path of
    vector_push_back(path_list, *uci);
    uci->option = "";
    *err = RE_OK;
    return path_list;
  } else if (yang_is_list(type)) {
    json_object_object_get_ex(node, YANG_MAP, &map);

    // If section is named
    if (!is_null_or_empty(uci->section)) {
      path_list = extract_list_paths(path_list, map, uci, err);
      if (*err != RE_OK) {
        return NULL;
      }
      goto done_list;
    }

    int list_length = uci_list_length(uci);
    int start = (uci->where) ? uci->index : 0;
    int end = (uci->where) ? uci->index + 1 : list_length;
    for (int index = start; index < end; index++) {
      uci->where = 1;
      uci->index = start;
      path_list = extract_list_paths(path_list, map, uci, err);
      if (*err != RE_OK) {
        return NULL;
      }
    }
    done_list:
    uci->option = "";
    uci->section_type = "";
    uci->index = 0;
    uci->where = 0;
    *err = RE_OK;
    return path_list;
  }
  json_object_object_get_ex(node, YANG_MAP, &map);
  path_list = extract_list_paths(path_list, map, uci, err);
  if (*err != RE_OK) {
    return NULL;
  }
  *err = RE_OK;
  return path_list;
}

int data_delete(struct CgiContext *cgi, char **pathvec, int root) {
  json_object *module = NULL;
  json_object *top_level = NULL;
  char *module_name = NULL;
  char *top_level_name = NULL;
  char *key = NULL;
  int retval = 1;
  struct UciPath uci = INIT_UCI_PATH();
  struct UciPath *delete = NULL;
  struct UciPath *delete_ref = NULL;
  char **package_list = NULL;
  error err;
  char exists_path[512];

  if (split_pair_by_char(pathvec[1], &module_name, &top_level_name, ':')) {
    retval = restconf_badrequest();
    goto done;
  }

  if (!(module = yang_module_exists(module_name))) {
    retval = restconf_unknown_namespace();
    goto done;
  }
  get_path_from_yang(module, &uci);

  top_level = json_get_object_from_map(module, top_level_name);
  if (!top_level) {
    retval = restconf_badrequest();
    goto done;
  }
  get_path_from_yang(top_level, &uci);

  err = check_path(&top_level, pathvec, 2, vector_size(pathvec), &uci, 1, 0, DELETE);
  if (!top_level || err != RE_OK) {
    retval = print_error(err);
    goto done;
  }
  uci_combine_to_path(&uci, exists_path, sizeof(exists_path));
  if (!uci_path_exists(exists_path)) {
    retval = restconf_no_such_element();
    goto done;
  }
  if (yang_mandatory(top_level)) {
    retval = restconf_badrequest();
    goto done;
  }

  if (vector_size(pathvec) > 3 && uci.parent && get_leaf_as_type(top_level, uci.parent)) {
    key = malloc(strlen(uci.section) + 1);
    if (key) {
      strcpy(key, uci.section);
      delete_ref = uci_dup_path(uci.parent);
    }
    uci.parent->option = "";
  }

  delete = extract_paths(top_level, &uci, &err);
  if (err != RE_OK) {
    retval = print_error(err);
    goto done;
  }
  for (size_t i = 0; i < vector_size(delete); i++) {
    char path_string[512];
    struct UciPath tmp = delete[i];

    uci_combine_to_path(&tmp, path_string, sizeof(path_string));

    if (!is_in_vector(package_list, tmp.package)) {
      vector_push_back(package_list, tmp.package);
    }

    if (uci_delete_path(path_string, false) == 1) {
      retval = print_error(INTERNAL);
      uci_revert_all(package_list);
      goto done;
    }
  }
  if (uci_commit_all(package_list)) {
    retval = restconf_partial_operation();
    goto done;
  }

  if (key && delete_ref) {
    char path_string[512];
    uci_combine_to_path(delete_ref, path_string, sizeof(path_string));
    if (uci_delete_list(path_string, key, true) == 1) {
      retval = print_error(INTERNAL);
      goto done;
    }
  }

  retval = 0;
  printf("Status: 204 No Content\r\n");
  headers_end();
done:
  if (key) {
    free(key);
  }
  if (delete) {
    vector_free(delete);
  }
  if (package_list) {
    vector_free(package_list);
  }
  return retval;
}