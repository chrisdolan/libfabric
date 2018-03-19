/*
 * Copyright (c) 2015 Cray Inc. All rights reserved.
 * Copyright (c) 2018 Intel Corp, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Copied from http://oopweb.com/Algorithms/Documents/Sman/VolumeFrames.html?/Algorithms/Documents/Sman/Volume/RedBlackTrees_files/s_rbt.htm
 *
 * Disclosure from the author's main page:
 * (http://oopweb.com/Algorithms/Documents/Sman/VolumeFrames.html?/Algorithms/Documents/Sman/Volume/RedBlackTrees_files/s_rbt.htm)
 *
 *     Source code when part of a software project may be used freely
 *     without reference to the author.
 *
 */

// reentrant red-black tree

#include <assert.h>

#include <ofi_tree.h>
#include <rdma/fi_errno.h>


void ofi_rbmap_init(struct ofi_rbmap *map)
{
	assert(map->compare);

	map->root = &map->sentinel;
	map->sentinel.left = &map->sentinel;
	map->sentinel.right = &map->sentinel;
	map->sentinel.parent = NULL;
	map->sentinel.color = BLACK;
	map->sentinel.data = NULL;
}

static void ofi_delete_tree(struct ofi_rbmap *map, struct ofi_rbnode *node)
{
	if (node == &map->sentinel)
		return;

	ofi_delete_tree(map, node->left);
	ofi_delete_tree(map, node->right);
	free(node);
}

void ofi_rbmap_cleanup(struct ofi_rbmap *map)
{
	ofi_delete_tree(map, map->root);
	free(map);
}

static void ofi_rotate_left(struct ofi_rbmap *map, struct ofi_rbnode *node)
{
	struct ofi_rbnode *y = node->right;

	node->right = y->left;
	if (y->left != &map->sentinel)
		y->left->parent = node;

	if (y != &map->sentinel)
		y->parent = node->parent;
	if (node->parent) {
		if (node== node->parent->left)
			node->parent->left = y;
		else
			node->parent->right = y;
	} else {
		map->root = y;
	}

	y->left = node;
	if (node != &map->sentinel)
		node->parent = y;
}

static void ofi_rotate_right(struct ofi_rbmap *map, struct ofi_rbnode *node)
{
	struct ofi_rbnode *y = node->left;

	node->left = y->right;
	if (y->right != &map->sentinel)
		y->right->parent = node;

	if (y != &map->sentinel)
		y->parent = node->parent;
	if (node->parent) {
		if (node == node->parent->right)
			node->parent->right = y;
		else
			node->parent->left = y;
	} else {
		map->root = y;
	}

	y->right = node;
	if (node != &map->sentinel)
		node->parent = y;
}

static void
ofi_insert_rebalance(struct ofi_rbmap *map, struct ofi_rbnode *x)
{
	struct ofi_rbnode *y;

	while (x != map->root && x->parent->color == RED) {
		if (x->parent == x->parent->parent->left) {
			y = x->parent->parent->right;
			if (y->color == RED) {
				x->parent->color = BLACK;
				y->color = BLACK;
				x->parent->parent->color = RED;
				x = x->parent->parent;
			} else {
				if (x == x->parent->right) {
					x = x->parent;
					ofi_rotate_left(map, x);
				}

				x->parent->color = BLACK;
				x->parent->parent->color = RED;
				ofi_rotate_right(map, x->parent->parent);
			}
		} else {
			y = x->parent->parent->left;
			if (y->color == RED) {
				x->parent->color = BLACK;
				y->color = BLACK;
				x->parent->parent->color = RED;
				x = x->parent->parent;
			} else {
				if (x == x->parent->left) {
					x = x->parent;
					ofi_rotate_right(map, x);
				}
				x->parent->color = BLACK;
				x->parent->parent->color = RED;
				ofi_rotate_left(map, x->parent->parent);
			}
		}
	}
	map->root->color = BLACK;
}

int ofi_rbmap_insert(struct ofi_rbmap *map, void *key, void *data)
{
	struct ofi_rbnode *current, *parent, *node;
	int ret;

	current = map->root;
	parent = NULL;

	while (current != &map->sentinel) {
		ret = map->compare(map, key, current->data);
		if (ret == 0)
			return -FI_EALREADY;

		parent = current;
		current = (ret < 0) ? current->left : current->right;
	}

	node = malloc(sizeof(*node));
	if (!node)
		return -FI_ENOMEM;

	node->parent = parent;
	node->left = &map->sentinel;
	node->right = &map->sentinel;
	node->color = RED;
	node->data = data;

	if (parent) {
		if (map->compare(map, key, parent->data) < 0)
			parent->left = node;
		else
			parent->right = node;
	} else {
		map->root = node;
	}

	ofi_insert_rebalance(map, node);
	return 0;
}

static void ofi_delete_rebalance(struct ofi_rbmap *map, struct ofi_rbnode *node)
{
	struct ofi_rbnode *w;

	while (node != map->root && node->color == BLACK) {
		if (node == node->parent->left) {
			w = node->parent->right;
			if (w->color == RED) {
				w->color = BLACK;
				node->parent->color = RED;
				ofi_rotate_left(map, node->parent);
				w = node->parent->right;
			}
			if (w->left->color == BLACK && w->right->color == BLACK) {
				w->color = RED;
				node = node->parent;
			} else {
				if (w->right->color == BLACK) {
					w->left->color = BLACK;
					w->color = RED;
					ofi_rotate_right(map, w);
					w = node->parent->right;
				}
				w->color = node->parent->color;
				node->parent->color = BLACK;
				w->right->color = BLACK;
				ofi_rotate_right(map, node->parent);
				node = map->root;
			}
		} else {
			w = node->parent->left;
			if (w->color == RED) {
				w->color = BLACK;
				node->parent->color = RED;
				ofi_rotate_right(map, node->parent);
				w = node->parent->left;
			}
			if (w->right->color == BLACK && w->left->color == BLACK) {
				w->color = RED;
				node = node->parent;
			} else {
				if (w->left->color == BLACK) {
					w->right->color = BLACK;
					w->color = RED;
					ofi_rotate_left(map, w);
					w = node->parent->left;
				}
				w->color = node->parent->color;
				node->parent->color = BLACK;
				w->left->color = BLACK;
				ofi_rotate_right(map, node->parent);
				node = map->root;
			}
		}
	}
	node->color = BLACK;
}

void ofi_rbmap_delete(struct ofi_rbmap *map, struct ofi_rbnode *node)
{
	struct ofi_rbnode *x, *y;

	if (node->left == &map->sentinel || node->right == &map->sentinel) {
		y = node;
	} else {
		y = node->right;
		while (y->left != &map->sentinel)
			y = y->left;
	}

	if (y->left != &map->sentinel)
		x = y->left;
	else
		x = y->right;

	x->parent = y->parent;
	if (y->parent) {
		if (y == y->parent->left)
			y->parent->left = x;
		else
			y->parent->right = x;
	} else {
		map->root = x;
	}

	if (y != node)
		node->data = y->data;

	if (y->color == BLACK)
		ofi_delete_rebalance(map, x);

	free (y);
}

struct ofi_rbnode *ofi_rbmap_find(struct ofi_rbmap *map, void *key)
{
	struct ofi_rbnode *node;
	int ret;

	node = map->root;
	while (node != &map->sentinel) {
		ret = map->compare(map, key, node->data);
		if (ret == 0)
			return node;

		node = (ret < 0) ? node->left : node->right;
	}
	return NULL;
}