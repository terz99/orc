#include <uci/cmd.h>
#include <uci/uci-get.h>
#include <uci/uci-util.h>
#include "restconf-json.h"
#include "vector.h"
#include "yang-verify.h"

/**
 * @brief verify a leaf list according to existence etc
 * @param value the leaf-list JSON array
 * @param path the UCI path pointing at that item
 * @param yang the YANG node for the leaf-list
 * @param check_exists check if node already exists
 * @return RE_OK if success else error
 */
static error _restconf_verify_leaf_list(struct json_object *value,
                                        struct UciPath *path,
                                        struct json_object *yang,
                                        int check_exists) {
  error err;
  if ((err = yang_verify_leaf_list(value, yang)) != RE_OK) {
    return err;
  }
  if (check_exists) {
    if (uci_element_exists(path)) {
      return ELEMENT_ALREADY_EXISTS;
    }
  }
  return RE_OK;
}

/**
 * @brief verify a leaf according to existence etc
 * @param value the leaf JSON object
 * @param path the UCI path pointing at that item
 * @param yang the YANG node for the leaf-
 * @param check_exists check if node already exists
 * @return RE_OK if success else error
 */
error restconf_verify_leaf(struct json_object *value, struct UciPath *path,
                           struct json_object *yang, int check_exists) {
  error err;
  if ((err = yang_verify_leaf(value, yang)) != RE_OK) {
    return err;
  }
  if (check_exists) {
    if (uci_element_exists(path)) {
      return ELEMENT_ALREADY_EXISTS;
    }
  }
  return RE_OK;
}

UciWritePair **restconf_verify_leaf_list(struct json_object *content,
                                         struct json_object *yang_node,
                                         UciWritePair **command_list,
                                         error *err, int check_exists,
                                         struct UciPath *path) {
  struct json_object *existing_items = NULL;

  json_type value_type = json_object_get_type(content);
  if (json_is_object(value_type)) {
    *err = INVALID_TYPE;
    return NULL;
  }
  if (check_exists) {
    if (!(existing_items = uci_get_leaf_list(yang_node, path, err))) {
      existing_items = json_object_new_array();
    }
  } else {
    existing_items = json_object_new_array();
  }
  if (*err != RE_OK && *err != UCI_READ_FAILED) {
    return NULL;
  }
  if (json_is_array(value_type)) {
    json_array_forloop(content, index) {
      struct json_object *tmp = json_object_array_get_idx(content, index);
      json_object_array_add(existing_items, tmp);
    }
  } else {
    json_object_array_add(existing_items, content);
  }
  if ((*err = _restconf_verify_leaf_list(existing_items, path, yang_node, 0)) !=
      RE_OK) {
    return NULL;
  }
  json_array_forloop(existing_items, index) {
    json_object *item = json_object_array_get_idx(existing_items, index);
    const char *value = json_object_get_string(item);
    UciWritePair *output = initialize_uci_write_pair(path, (char *)value, list);
    if (!output) {
      *err = INTERNAL;
      return NULL;
    }
    vector_push_back(command_list, output);
  }
  return command_list;
}

static char *get_reference_name(struct UciPath *path, const char *string) {
  int size = sizeof(char) * (strlen(path->section) + strlen(string) + 2);
  char *res = (char *) malloc(size);
  if (!res) {
    return NULL;
  }
  size = snprintf(res, size, "%s_%s", path->section, string);
  return res;
}

UciWritePair **restconf_verify_nested_lists(struct json_object *content,
                                            struct json_object *yang_node,
                                            struct UciPath *path, error *err) {
  UciWritePair **command_list = NULL;
  if (json_object_get_type(content) == json_type_array) {
    json_array_forloop(content, index) {
      struct json_object *item = json_object_array_get_idx(content, index);
      const char *v = get_leaf_as_name_value(yang_node, item);
      char *value = get_reference_name(path, v);
      if (!value) {
        *err = INTERNAL;
        return NULL;
      }

      UciWritePair *output = initialize_uci_write_pair(path, value, list);
      if (!output) {
        *err = INTERNAL;
        return NULL;
      }

      vector_push_back(command_list, output);
    }
  } else if (json_object_get_type(content) == json_type_object) {
    char *value = (char *) get_leaf_as_name_value(yang_node, content);
    UciWritePair *output = initialize_uci_write_pair(path, value, list);
    if (!output) {
      *err = INTERNAL;
      return NULL;
    }

    vector_push_back(command_list, output);
  } else {
    *err = INVALID_TYPE;
    return NULL;
  }

  *err = RE_OK;
  return command_list;
}