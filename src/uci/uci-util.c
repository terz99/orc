#include "uci/uci-util.h"
#include <restconf.h>
#include "http.h"
#include "restconf-json.h"
#include "vector.h"
#include "yang-util.h"

/**
 * Combines the uci_path struct to a string
 * @param path the uci_path* to be combined
 * @param buffer char* into which the output should be copied
 * @param size the size of the buffer
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
int combine_to_path(struct UciPath *path, char *buffer, size_t size) {
  int retval;
  if (!path->option || strlen(path->option) == 0) {
    retval = snprintf(buffer, size, "%s.%s", path->package, path->section);
  } else {
    retval = snprintf(buffer, size, "%s.%s.%s", path->package, path->section,
                      path->option);
  }
  if (retval < 1) {
    return 1;
  } else {
    return 0;
  }
}

/**
 * Combines the uci_path struct and index into an anonymous section path
 * @param path the uci_path* to be combined
 * @param index the index of the anonymous section
 * @param buffer char* into which the output should be copied
 * @param size the size of the buffer
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
int combine_to_anonymous_path(struct UciPath *path, int index, char *buffer,
                              size_t size) {
  int retval;
  if (!path->option || strlen(path->option) == 0) {
    retval = snprintf(buffer, size, "%s.@%s[%d]", path->package,
                      path->section_type, index);
  } else {
    retval = snprintf(buffer, size, "%s.@%s[%d].%s", path->package,
                      path->section_type, index, path->option);
  }
  if (retval < 1) {
    return 1;
  } else {
    return 0;
  }
}

/**
 * Combines uci_path to string UCI path
 * @param path the path object
 * @param buffer the output value
 * @param buffer_size the size of the output buffer
 * @return 0
 */
int uci_combine_to_path(struct UciPath *path, char *buffer,
                        size_t buffer_size) {
  if (path->where && (path->section == NULL || strlen(path->section) == 0)) {
    combine_to_anonymous_path(path, path->index, buffer, buffer_size);
  } else {
    combine_to_path(path, buffer, buffer_size);
  }
  return 0;
}

int get_path_from_yang(struct json_object *jobj, struct UciPath *uci) {
  struct json_object *uci_value = NULL;
  char *uci_package = NULL;
  char *uci_section = NULL;
  char *uci_option = NULL;
  char *uci_section_type = NULL;
  char *uci_parent = NULL;
  int uci_depth = -1;
  json_object_object_get_ex(jobj, YANG_UCI_PACKAGE, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    uci_package = (char *)json_object_get_string(uci_value);
  }
  json_object_object_get_ex(jobj, YANG_UCI_SECTION_NAME, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    uci_section = (char *)json_object_get_string(uci_value);
  }
  json_object_object_get_ex(jobj, YANG_UCI_OPTION, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    uci_option = (char *)json_object_get_string(uci_value);
  }
  json_object_object_get_ex(jobj, YANG_UCI_SECTION, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    uci_section_type = (char *)json_object_get_string(uci_value);
  }
  json_object_object_get_ex(jobj, YANG_DEPTH, &uci_value);
  if (json_object_get_type(uci_value) == json_type_int) {
    uci_depth = (int) json_object_get_int(uci_value);
  }

  struct UciPath *path_dup = uci_dup_path(uci);
  if (!path_dup) {
    goto done;
  }
  if (uci_depth != -1) {
    uci->depth = uci_depth;
  }
  if (uci_package) {
    if (uci_section && uci_section_type) {
      if (uci_option) {
        uci->package = uci_package;
        uci->section = uci_section;
        uci->section_type = uci_section_type;
        uci->option = uci_option;
        goto done;
      }
      uci->package = uci_package;
      uci->section = uci_section;
      uci->section_type = uci_section_type;
      uci->option = "";
      goto done;
    } else if (uci_section) {
      if (uci_option) {
        uci->package = uci_package;
        uci->section = uci_section;
        uci->section_type = "container";
        uci->option = uci_option;
        goto done;
      }
      uci->package = uci_package;
      uci->section = uci_section;
      uci->section_type = "container";
      uci->option = "";
      goto done;
    }
    uci->package = uci_package;
    uci->section = "";
    uci->section_type = "";
    uci->option = "";
    goto done;
  }

  if (uci_section && uci_section_type) {
    if (uci_option) {
      uci->section = uci_section;
      uci->section_type = uci_section_type;
      uci->option = uci_option;
      goto done;
    }
    uci->section = uci_section;
    uci->section_type = uci_section_type;
    uci->option = "";
    goto done;
  } else if (uci_section) {
    if (uci_option) {
      uci->section = uci_section;
      uci->section_type = "container";
      uci->option = uci_option;
      goto done;
    }
    uci->section = uci_section;
    uci->section_type = "container";
    uci->option = "";
    goto done;
  } else if (uci_section_type) {
    uci->section_type = uci_section_type;
    uci->section = "";
    uci->option = "";
    uci->index = 0;
    uci->where = 0;
  }

  if (uci_option) {
    uci->option = uci_option;
  }
  done:
  json_object_object_get_ex(jobj, YANG_PARENT, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    uci_parent = (char *) json_object_get_string(uci_value);
  }
  if (uci_parent) {
    while (path_dup && !(strcmp(path_dup->section_type, uci_parent) == 0 && path_dup->depth == uci->depth - 1)) {
      path_dup = path_dup->parent;
    }
    if (path_dup) {
      uci->parent = uci_link_parent(path_dup);
    } else {
      uci->parent = NULL;
    }
  }
  return 0;
}

int get_leaf_as_name(struct json_object *yang, struct json_object *json,
                     struct UciPath *uci) {
  const char *value = NULL;
  if (!(value = get_leaf_as_name_value(yang, json))) {
    return 0;
  }
  if (uci->parent != NULL) {
    int size = strlen(value) + strlen(uci->parent->section) + 2;
    char *buf = malloc(size);
    if (!buf) {
      return 1;
    }
    snprintf(buf, size, "%s_%s", uci->parent->section, value);
    uci->section = buf;
  } else {
    uci->section = (char *) value;
  }
  return 0;
}

const char *get_leaf_as_name_value(struct json_object *yang, struct json_object *json) {
  struct json_object *uci_value = NULL;
  char *uci_leaf_as_name = NULL;
  json_object_object_get_ex(yang, YANG_UCI_LEAF_AS_NAME, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    uci_leaf_as_name = (char *)json_object_get_string(uci_value);
  } else {
    return NULL;
  }
  json_object_object_get_ex(json, uci_leaf_as_name, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    return json_object_get_string(uci_value);
  }
  return NULL;
}

int get_leaf_as_type(struct json_object *yang, struct UciPath *uci) {
  struct json_object *uci_value = NULL;
  char *uci_leaf_as_type = NULL;
  json_object_object_get_ex(yang, YANG_UCI_LEAF_AS_TYPE, &uci_value);
  if (json_object_get_type(uci_value) == json_type_string) {
    uci_leaf_as_type = (char *)json_object_get_string(uci_value);
    uci->option = uci_leaf_as_type;
    return 1;
  }

  return 0;
}

int uci_revert_all(char **package_list) {
  for (size_t i = 0; i < vector_size(package_list); i++) {
    char *item = package_list[i];
    if (uci_revert_package(item)) {
      return 1;
    }
  }
  return 0;
}

int uci_commit_all(char **package_list) {
  for (size_t i = 0; i < vector_size(package_list); i++) {
    char *item = package_list[i];
    if (uci_commit_package(item)) {
      return 1;
    }
  }
  return 0;
}

int uci_element_exists(struct UciPath *path) {
  char path_string[512];
  uci_combine_to_path(path, path_string, sizeof(path_string));
  if (uci_path_exists(path_string)) {
    return 1;
  }
  return 0;
}