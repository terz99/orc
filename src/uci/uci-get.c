#include "cmd.h"
#include "error.h"
#include "http.h"
#include "restconf-json.h"
#include "restconf-method.h"
#include "uci/uci-util.h"
#include "vector.h"
#include "yang-util.h"

int add_to_json(struct json_object *check, struct json_object *top_level,
                char *key, const char *type, error *err) {

  if (!check && *err != RE_OK && *err != UCI_READ_FAILED &&
      *err != NO_SUCH_ELEMENT) {
    return 0;
  }
  if (*err != UCI_READ_FAILED && check) {
    struct json_object *array = NULL;
    if ((array = json_object_object_get(top_level, key))
      && json_object_get_type(array) == json_type_array
      && json_object_get_type(check) == json_type_array) {
      for (size_t i = 0; i < json_object_array_length(check); i++) {
        json_object_array_add(array, json_object_array_get_idx(check, i));
      }
    } else {
      json_object_object_add(top_level, key, check);
    }
  } else if (!check && err == RE_OK && type != NULL && yang_is_container(type)) {
    json_object_object_add(top_level, key, json_object_new_object());
  }
  return 1;
}

char **uci_get_children_references(struct UciPath *path, error *err) {
  char **ref_names = NULL;
  char path_string[512];

  uci_combine_to_path(path, path_string, sizeof(path_string));
  if (!(ref_names = uci_read_list(path_string))) {
    *err = UCI_READ_FAILED;
    return NULL;
  }

  *err = RE_OK;
  return ref_names;
}

struct json_object *uci_get_list(struct json_object *yang, struct UciPath *path,
                                 error *err) {
  struct json_object *map = NULL;
  struct json_object *array = NULL;
  struct json_object *top_level = NULL;
  struct json_object *check = NULL;
  const char *type = NULL;
  int list_length;
  int single_item = path->where;
  error err_rec;

  if (!(type = json_get_string(yang, YANG_TYPE))) {
    *err = YANG_SCHEMA_ERROR;
    return NULL;
  }

  if (strlen(path->section_type) != 0 && strlen(path->section) != 0) {
    const char *type_string = NULL;
    path->where = 0;
    array = json_object_new_array();
    top_level = json_object_new_object();
    json_object_object_get_ex(yang, YANG_MAP, &map);
    json_object_object_foreach(map, key, val) {
      err_rec = RE_OK;
      if (!(type_string = json_get_string(val, YANG_TYPE))) {
        *err = YANG_SCHEMA_ERROR;
        return NULL;
      }
      if (yang_is_container(type_string) || yang_is_list(type_string)) {
        continue;
      }
      check = build_recursive(val, path, &err_rec, NULL, 0);
      if (!add_to_json(check, top_level, key, NULL, &err_rec)) {
        *err = err_rec;
        return NULL;
      }
    }

    json_object_object_foreach(map, object_key, value) {
      err_rec = RE_OK;
      if (!(type_string = json_get_string(value, YANG_TYPE))) {
        *err = YANG_SCHEMA_ERROR;
        return NULL;
      }
      if (yang_is_leaf_list(type_string) || yang_is_leaf(type_string)) {
        continue;
      }

      if (get_leaf_as_type(value, path)) {
        char **ref_names = uci_get_children_references(path, err);
        for (size_t i = 0; i < vector_size(ref_names); i++) {
          check = build_recursive(value, path, &err_rec, ref_names[i], 0);
          if (!add_to_json(check, top_level, object_key, type, &err_rec)) {
            *err = err_rec;
            return NULL;
          }
        }
      } else {
        check = build_recursive(value, path, &err_rec, NULL, 0);
        if (!add_to_json(check, top_level, object_key, type, &err_rec)) {
          *err = err_rec;
          return NULL;
        }
      }
    }
    json_object_array_add(array, top_level);
    goto done;
  }

  if (strlen(path->section_type) == 0 || strlen(path->section) != 0) {
    *err = YANG_SCHEMA_ERROR;
    return NULL;
  }

  json_object_object_get_ex(yang, YANG_MAP, &map);

  if ((list_length = uci_list_length(path)) < 1) {
    *err = RE_OK;
    return NULL;
  }

  array = json_object_new_array();
  for (int index = 0; index < list_length; index++) {
    if (!single_item) {
      path->index = index;
      path->where = 1;
    }
    top_level = json_object_new_object();
    json_object_object_foreach(map, key, val) {
      err_rec = RE_OK;
      const char *type_string = NULL;
      if (!(type_string = json_get_string(val, YANG_TYPE))) {
        *err = YANG_SCHEMA_ERROR;
        return NULL;
      }
      if (yang_is_list(type_string) || yang_is_container(type_string)) {
        continue;
      }
      check = build_recursive(val, path, &err_rec, NULL, 0);
      if (!add_to_json(check, top_level, key, NULL, &err_rec)) {
        *err = err_rec;
        return NULL;
      }
    }

    json_object_object_foreach(map, object_key, value) {
      err_rec = RE_OK;
      const char *type_string = NULL;
      if (!(type_string = json_get_string(value, YANG_TYPE))) {
        *err = YANG_SCHEMA_ERROR;
        return NULL;
      }
      if (yang_is_leaf_list(type_string) || yang_is_leaf(type_string)) {
        continue;
      }

      if (get_leaf_as_type(value, path)) {
        char **ref_names = uci_get_children_references(path, err);
        for (size_t i = 0; i < vector_size(ref_names); i++) {
          check = build_recursive(value, path, &err_rec, ref_names[i], 0);
          if (!add_to_json(check, top_level, object_key, type, &err_rec)) {
            *err = err_rec;
            return NULL;
          }
        }
      } else {
        check = build_recursive(value, path, &err_rec, NULL, 0);
        if (!add_to_json(check, top_level, object_key, type, &err_rec)) {
          *err = err_rec;
          return NULL;
        }
      }
    }
    json_object_array_add(array, top_level);
    if (single_item) {
      break;
    }
  }

  done:
  *err = RE_OK;
  path->index = 0;
  path->where = 0;
  if (json_object_array_length(array) == 0) {
    return NULL;
  }
  return array;
}

struct json_object *uci_get_leaf(struct json_object *yang, struct UciPath *path,
                                 error *err) {
  char path_string[512], buf[512];
  yang_type leaf_type;
  struct json_object *output = NULL;
  struct json_object *type = NULL;

  uci_combine_to_path(path, path_string, sizeof(path_string));
  if (uci_read_option(path_string, buf, sizeof(buf))) {
    *err = UCI_READ_FAILED;
    goto done;
  }
  json_object_object_get_ex(yang, YANG_LEAF_TYPE, &type);
  if (!type) {
    *err = YANG_SCHEMA_ERROR;
    goto done;
  }
  if ((leaf_type = json_extract_yang_type(type)) == NONE) {
    // TODO: Solve this problem
    //    *err = YANG_SCHEMA_ERROR;
    //    goto done;
    leaf_type = STRING;
  }
  if (!(output = json_yang_type_format(leaf_type, buf))) {
    *err = YANG_SCHEMA_ERROR;
    goto done;
  }
  *err = RE_OK;
done:
  path->option = "";
  return output;
}

struct json_object *uci_get_leaf_list(struct json_object *yang,
                                      struct UciPath *path, error *err) {
  char path_string[512];
  char **items = NULL;
  yang_type leaf_type;
  struct json_object *output = NULL;
  struct json_object *type = NULL;

  uci_combine_to_path(path, path_string, sizeof(path_string));
  if (!(items = uci_read_list(path_string))) {
    *err = UCI_READ_FAILED;
    goto done;
  }
  json_object_object_get_ex(yang, YANG_LEAF_TYPE, &type);
  if (!type) {
    *err = YANG_SCHEMA_ERROR;
    goto done;
  }
  if ((leaf_type = json_extract_yang_type(type)) == NONE) {
    *err = YANG_SCHEMA_ERROR;
    goto done;
  }
  output = json_object_new_array();
  for (size_t i = 0; i < vector_size(items); i++) {
    struct json_object *iter = NULL;
    iter = json_yang_type_format(leaf_type, items[i]);
    if (!iter) {
      *err = YANG_SCHEMA_ERROR;
      goto done;
    }
    json_object_array_add(output, iter);
  }
  vector_free(items);
  *err = RE_OK;
done:
  return output;
}