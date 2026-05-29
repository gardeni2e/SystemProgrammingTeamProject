#ifndef MENU_H
#define MENU_H

#include "common.h"

void menu_init_catalog(MenuCatalog *cat);
int menu_load_file(MenuCatalog *cat, const char *path, char *err, size_t errsz);
int menu_save_file(const MenuCatalog *cat, const char *path, char *err,
                   size_t errsz);

const MenuItem *menu_find_by_id(const MenuCatalog *cat, int id);
int menu_index_by_id(const MenuCatalog *cat, int id);

int menu_add_item(MenuCatalog *cat, const MenuItem *item, char *err,
                  size_t errsz);
int menu_update_item(MenuCatalog *cat, int id, const MenuItem *item, char *err,
                     size_t errsz);
int menu_delete_item(MenuCatalog *cat, int id, char *err, size_t errsz);
int menu_set_soldout(MenuCatalog *cat, int id, int sold_out, char *err,
                     size_t errsz);
int menu_set_category(MenuCatalog *cat, int id, const char *category,
                      char *err, size_t errsz);

int menu_set_popular(MenuCatalog *cat, int id, int popular,
                     char *err, size_t errsz);

#endif
