/* Internationalization Tag Set (ITS) handling
   Copyright (C) 2015 Free Software Foundation, Inc.

   This file was written by Daiki Ueno <ueno@gnu.org>, 2015.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* Specification.  */
#include "its.h"

#include <assert.h>
#include <errno.h>
#include "error.h"
#include "gettext.h"
#include "hash.h"
#include <stdint.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <stdlib.h>
#include "xalloc.h"

#define _(str) gettext (str)

/* The Internationalization Tag Set (ITS) 2.0 standard is published at:
   http://www.w3.org/TR/its20/

   This implementation supports only a few data categories, useful for
   gettext-based projects.  Other data categories can be added by
   extending a class from its_rule_class_ty and registering it in
   init_classes().

   The value associated with a data category is represented as an
   array of key-value pairs.  */

#define ITS_NS "http://www.w3.org/2005/11/its"

struct its_value_ty
{
  char *name;
  char *value;
};

struct its_value_list_ty
{
  struct its_value_ty *items;
  size_t nitems;
  size_t nitems_max;
};

static void
its_value_list_append (struct its_value_list_ty *values,
                       const char *name,
                       const char *value)
{
  struct its_value_ty _value;

  _value.name = xstrdup (name);
  _value.value = xstrdup (value);

  if (values->nitems == values->nitems_max)
    {
      values->nitems_max = 2 * values->nitems_max + 1;
      values->items =
        xrealloc (values->items,
                  sizeof (struct its_value_ty) * values->nitems_max);
    }
  memcpy (&values->items[values->nitems++], &_value,
          sizeof (struct its_value_ty));
}

static const char *
its_value_list_get_value (struct its_value_list_ty *values,
                          const char *name)
{
  size_t i;

  for (i = 0; i < values->nitems; i++)
    {
      struct its_value_ty *value = &values->items[i];
      if (strcmp (value->name, name) == 0)
        return value->value;
    }
  return NULL;
}

static void
its_value_list_merge (struct its_value_list_ty *values,
                      struct its_value_list_ty *other)
{
  size_t i;

  for (i = 0; i < other->nitems; i++)
    {
      struct its_value_ty *other_value = &other->items[i];
      size_t j;

      for (j = 0; j < values->nitems; j++)
        {
          struct its_value_ty *value = &values->items[j];

          if (strcmp (value->name, other_value->name) == 0
              && strcmp (value->value, other_value->value) != 0)
            {
              free (value->value);
              value->value = xstrdup (other_value->value);
              break;
            }
        }

      if (j == values->nitems)
        its_value_list_append (values, other_value->name, other_value->value);
    }
}

static void
its_value_list_destroy (struct its_value_list_ty *values)
{
  size_t i;

  for (i = 0; i < values->nitems; i++)
    {
      free (values->items[i].name);
      free (values->items[i].value);
    }
  free (values->items);
}

struct its_pool_ty
{
  struct its_value_list_ty *items;
  size_t nitems;
  size_t nitems_max;
};

static struct its_value_list_ty *
its_pool_alloc_value_list (struct its_pool_ty *pool)
{
  struct its_value_list_ty *values;

  if (pool->nitems == pool->nitems_max)
    {
      pool->nitems_max = 2 * pool->nitems_max + 1;
      pool->items =
        xrealloc (pool->items,
                  sizeof (struct its_value_list_ty) * pool->nitems_max);
    }

  values = &pool->items[pool->nitems++];
  memset (values, 0, sizeof (struct its_value_list_ty));
  return values;
}

static void
its_pool_destroy (struct its_pool_ty *pool)
{
  size_t i;

  for (i = 0; i < pool->nitems; i++)
    its_value_list_destroy (&pool->items[i]);
  free (pool->items);
}

struct its_rule_list_ty
{
  struct its_rule_ty **items;
  size_t nitems;
  size_t nitems_max;

  struct its_pool_ty pool;
};

struct its_node_list_ty
{
  xmlNode **items;
  size_t nitems;
  size_t nitems_max;
};

static void
its_node_list_append (struct its_node_list_ty *nodes,
                      xmlNode *node)
{
  if (nodes->nitems == nodes->nitems_max)
    {
      nodes->nitems_max = 2 * nodes->nitems_max + 1;
      nodes->items =
        xrealloc (nodes->items, sizeof (xmlNode *) * nodes->nitems_max);
    }
  nodes->items[nodes->nitems++] = node;
}

/* Base class representing an ITS rule in global definition.  */
struct its_rule_class_ty
{
  /* How many bytes to malloc for an instance of this class.  */
  size_t size;

  /* What to do immediately after the instance is malloc()ed.  */
  void (*constructor) (struct its_rule_ty *pop, xmlNode *node);

  /* What to do immediately before the instance is free()ed.  */
  void (*destructor) (struct its_rule_ty *pop);

  /* How to apply the rule to all elements in DOC.  */
  void (* apply) (struct its_rule_ty *pop, struct its_pool_ty *pool,
                  xmlDoc *doc);

  /* How to evaluate the value of NODE according to the rule.  */
  struct its_value_list_ty *(* eval) (struct its_rule_ty *pop,
                                      struct its_pool_ty *pool, xmlNode *node);
};

#define ITS_RULE_TY                             \
  struct its_rule_class_ty *methods;            \
  char *selector;                               \
  struct its_value_list_ty values;

struct its_rule_ty
{
  ITS_RULE_TY
};

static hash_table classes;

static void
its_rule_destructor (struct its_rule_ty *pop)
{
  free (pop->selector);
  its_value_list_destroy (&pop->values);
}

static void
its_rule_apply (struct its_rule_ty *rule, struct its_pool_ty *pool, xmlDoc *doc)
{
  xmlXPathContext *context;
  xmlXPathObject *object;
  size_t i;

  if (!rule->selector)
    {
      error (0, 0, _("selector is not specified"));
      return;
    }

  context = xmlXPathNewContext (doc);
  if (!context)
    {
      error (0, 0, _("cannot create XPath context"));
      return;
    }

  object = xmlXPathEvalExpression (BAD_CAST rule->selector, context);
  if (!object)
    {
      xmlXPathFreeContext (context);
      error (0, 0, _("cannot evaluate XPath expression: %s"), rule->selector);
      return;
    }

  if (object->nodesetval)
    {
      xmlNodeSet *nodes = object->nodesetval;
      for (i = 0; i < nodes->nodeNr; i++)
        {
          xmlNode *node = nodes->nodeTab[i];
          struct its_value_list_ty *values;

          /* We can't store VALUES in NODE, since the address can
             change when realloc()ed.  */
          intptr_t index = (intptr_t) node->_private;

          assert (index <= pool->nitems);
          if (index > 0)
            values = &pool->items[index - 1];
          else
            {
              values = its_pool_alloc_value_list (pool);
              node->_private = (void *) pool->nitems;
            }

          its_value_list_merge (values, &rule->values);
        }
    }

  xmlXPathFreeObject (object);
  xmlXPathFreeContext (context);
}

static char *
_its_get_attribute (xmlNode *node, const char *attr)
{
  xmlChar *value;
  char *result;

  value = xmlGetProp (node, BAD_CAST attr);

  result = xstrdup ((const char *) value);
  xmlFree (value);

  return result;
}

static const char *
_its_collect_text_content (xmlNode *node)
{
  static char *buffer;
  static size_t bufmax;
  size_t bufpos = 0;
  xmlNode *n;

  for (n = node->children; n; n = n->next)
    if (n->type == XML_TEXT_NODE)
      {
        xmlChar *content = xmlNodeGetContent (n);
        size_t content_length = xmlStrlen (content);

        if (bufpos >= bufmax)
          {
            bufmax = 2 * bufmax + content_length + 1;
            buffer = xrealloc (buffer, bufmax);
          }
        memcpy (&buffer[bufpos], content, content_length);
        bufpos += content_length;
        buffer[bufpos] = 0;
      }
  return buffer;
}

/* Implementation of Translate data category.  */
static void
its_translate_rule_constructor (struct its_rule_ty *pop, xmlNode *node)
{
  char *prop;

  if (!xmlHasProp (node, BAD_CAST "selector"))
    {
      error (0, 0, _("\"translateRule\" node does not contain \"selector\""));
      return;
    }

  if (!xmlHasProp (node, BAD_CAST "translate"))
    {
      error (0, 0, _("\"translateRule\" node does not contain \"translate\""));
      return;
    }

  prop = _its_get_attribute (node, "selector");
  if (prop)
    pop->selector = prop;

  prop = _its_get_attribute (node, "translate");
  its_value_list_append (&pop->values, "translate", prop);
  free (prop);
}

struct its_value_list_ty *
its_translate_rule_eval (struct its_rule_ty *pop, struct its_pool_ty *pool,
                         xmlNode *node)
{
  struct its_value_list_ty *result;
  xmlNode *n = node;

  result = XCALLOC (1, struct its_value_list_ty);

  /* Inherit from the parent elements.  */
  for (n = node; n && n->type == XML_ELEMENT_NODE; n = n->parent)
    if ((intptr_t) n->_private > 0)
      break;

  if (n == NULL || (intptr_t) n->_private == 0)
    /* The default value is translate="yes".  */
    its_value_list_append (result, "translate", "yes");
  else
    {
      intptr_t index = (intptr_t) n->_private;
      struct its_value_list_ty *values;

      assert (index <= pool->nitems);
      values = &pool->items[index - 1];
      its_value_list_merge (result, values);
    }

  return result;
}

static struct its_rule_class_ty its_translate_rule_class =
  {
    sizeof (struct its_rule_ty),
    its_translate_rule_constructor,
    its_rule_destructor,
    its_rule_apply,
    its_translate_rule_eval,
  };

/* Implementation of Localization Note data category.  */
static void
its_localization_note_rule_constructor (struct its_rule_ty *pop, xmlNode *node)
{
  char *prop;
  xmlNode *n;

  if (!xmlHasProp (node, BAD_CAST "selector"))
    {
      error (0, 0, _("\"locNoteRule\" node does not contain \"selector\""));
      return;
    }

  if (!xmlHasProp (node, BAD_CAST "locNoteType"))
    {
      error (0, 0, _("\"locNoteRule\" node does not contain \"locNoteType\""));
      return;
    }

  prop = _its_get_attribute (node, "selector");
  if (prop)
    pop->selector = prop;

  for (n = node->children; n; n = n->next)
    {
      if (n->type == XML_ELEMENT_NODE
          && xmlStrEqual (n->name, BAD_CAST "locNote")
          && xmlStrEqual (n->ns->href, BAD_CAST ITS_NS))
        break;
    }

  if (n)
    its_value_list_append (&pop->values, "locNote",
                           _its_collect_text_content (n));
  else if (xmlHasProp (node, BAD_CAST "locNotePointer"))
    {
      prop = _its_get_attribute (node, "locNotePointer");
      its_value_list_append (&pop->values, "locNotePointer", prop);
      free (prop);
    }
}

struct its_value_list_ty *
its_localization_note_rule_eval (struct its_rule_ty *pop,
                                 struct its_pool_ty *pool,
                                 xmlNode *node)
{
  struct its_value_list_ty *result;
  xmlNode *n = node;

  result = XCALLOC (1, struct its_value_list_ty);

  /* Inherit from the parent elements.  */
  for (n = node; n && n->type == XML_ELEMENT_NODE; n = n->parent)
    if ((intptr_t) n->_private > 0)
      break;

  /* The default value is None.  */
  if (n != NULL && (intptr_t) n->_private > 0)
    {
      intptr_t index = (intptr_t) n->_private;
      struct its_value_list_ty *values;

      assert (index <= pool->nitems);
      values = &pool->items[index - 1];
      its_value_list_merge (result, values);
    }

  return result;
}

static struct its_rule_class_ty its_localization_note_rule_class =
  {
    sizeof (struct its_rule_ty),
    its_localization_note_rule_constructor,
    its_rule_destructor,
    its_rule_apply,
    its_localization_note_rule_eval,
  };

static struct its_rule_ty *
its_rule_alloc (struct its_rule_class_ty *method_table, xmlNode *node)
{
  struct its_rule_ty *pop;

  pop = (struct its_rule_ty *) xcalloc (1, method_table->size);
  pop->methods = method_table;
  if (method_table->constructor)
    method_table->constructor (pop, node);
  return pop;
}

static struct its_rule_ty *
its_rule_parse (xmlNode *node)
{
  const char *name = (const char *) node->name;
  void *value;

  if (hash_find_entry (&classes, name, strlen (name), &value) == 0)
    return its_rule_alloc ((struct its_rule_class_ty *) value, node);

  return NULL;
}

static void
its_rule_destroy (struct its_rule_ty *pop)
{
  if (pop->methods->destructor)
    pop->methods->destructor (pop);
}

static void
init_classes (void)
{
#define ADD_RULE_CLASS(n, c) \
  hash_insert_entry (&classes, n, strlen (n), &c);

  ADD_RULE_CLASS ("translateRule", its_translate_rule_class);
  ADD_RULE_CLASS ("locNoteRule", its_localization_note_rule_class);

#undef ADD_RULE_CLASS
}

struct its_rule_list_ty *
its_rule_list_alloc (void)
{
  struct its_rule_list_ty *result;

  if (classes.table == NULL)
    {
      hash_init (&classes, 10);
      init_classes ();
    }

  result = XCALLOC (1, struct its_rule_list_ty);
  return result;
}

void
its_rule_list_free (struct its_rule_list_ty *rules)
{
  size_t i;

  for (i = 0; i < rules->nitems; i++)
    its_rule_destroy (rules->items[i]);
  free (rules->items);
  its_pool_destroy (&rules->pool);
}

bool
its_rule_list_add_file (struct its_rule_list_ty *rules,
                        const char *filename)
{
  xmlDoc *doc;
  xmlNode *root, *node;
  FILE *fp;

  fp = fopen (filename, "r");
  if (fp == NULL)
    {
      error (0, errno,
             _("error while opening \"%s\" for reading"), filename);
      return false;
    }

  doc = xmlReadFd (fileno (fp), filename, "utf-8",
                   XML_PARSE_NONET
                   | XML_PARSE_NOWARNING
                   | XML_PARSE_NOBLANKS
                   | XML_PARSE_NOERROR);
  fclose (fp);
  if (doc == NULL)
    return false;

  root = xmlDocGetRootElement (doc);
  if (!(xmlStrEqual (root->name, BAD_CAST "rules")
        && xmlStrEqual (root->ns->href, BAD_CAST ITS_NS)))
    {
      error (0, 0, _("the root element is not \"rules\""
                     " under namespace %s"),
             ITS_NS);
      xmlFreeDoc (doc);
      return false;
    }

  for (node = root->children; node; node = node->next)
    {
      struct its_rule_ty *rule;

      rule = its_rule_parse (node);
      if (!rule)
        {
          xmlFreeDoc (doc);
          return false;
        }

      if (rules->nitems == rules->nitems_max)
        {
          rules->nitems_max = 2 * rules->nitems_max + 1;
          rules->items =
            xrealloc (rules->items,
                      sizeof (struct its_rule_ty *) * rules->nitems_max);
        }
      rules->items[rules->nitems++] = rule;
    }

  return true;
}

static void
its_rule_list_apply (struct its_rule_list_ty *rules, xmlDoc *doc)
{
  size_t i;

  for (i = 0; i < rules->nitems; i++)
    {
      struct its_rule_ty *rule = rules->items[i];
      rule->methods->apply (rule, &rules->pool, doc);
    }
}

static struct its_value_list_ty *
its_rule_list_eval (its_rule_list_ty *rules, xmlNode *node)
{
  struct its_value_list_ty *result;
  size_t i;

  result = XCALLOC (1, struct its_value_list_ty);
  for (i = 0; i < rules->nitems; i++)
    {
      struct its_rule_ty *rule = rules->items[i];
      struct its_value_list_ty *values;

      values = rule->methods->eval (rule, &rules->pool, node);
      its_value_list_merge (result, values);
      its_value_list_destroy (values);
      free (values);
    }

  return result;
}

static void
its_rule_list_extract_nodes (its_rule_list_ty *rules,
                             struct its_node_list_ty *nodes,
                             xmlNode *node,
                             const struct its_value_ty *values)
{
  if (node->type == XML_ELEMENT_NODE)
    {
      struct its_value_list_ty *element_values;
      size_t i;
      xmlNode *n;

      element_values = its_rule_list_eval (rules, node);
      for (i = 0; values[i].name != NULL; i++)
        {
          size_t j;

          for (j = 0; j < element_values->nitems; j++)
            {
              struct its_value_ty *element_value = &element_values->items[j];
              if (strcmp (values[i].name, element_value->name) == 0
                  && strcmp (values[i].value, element_value->value) == 0)
                break;
            }

          if (j == element_values->nitems)
            break;
        }

      if (values[i].name == NULL)
        its_node_list_append (nodes, node);

      for (n = node->children; n; n = n->next)
        its_rule_list_extract_nodes (rules, nodes, n, values);
    }
}

static char *
_its_get_content (xmlNode *node, const char *pointer)
{
  xmlXPathContext *context;
  xmlXPathObject *object;
  char *result;

  context = xmlXPathNewContext (node->doc);
  if (!context)
    {
      error (0, 0, _("cannot create XPath context"));
      return NULL;
    }

  object = xmlXPathNodeEval (node, BAD_CAST pointer, context);
  if (!object)
    {
      xmlXPathFreeContext (context);
      error (0, 0, _("cannot evaluate XPath location path: %s"),
             pointer);
      return NULL;
    }

  if (object->nodesetval)
    {
      xmlNodeSet *nodes = object->nodesetval;
      string_list_ty sl;
      size_t i;

      string_list_init (&sl);
      for (i = 0; i < nodes->nodeNr; i++)
        string_list_append (&sl, _its_collect_text_content (nodes->nodeTab[i]));
      result = string_list_concat (&sl);
    }

  xmlXPathFreeObject (object);
  xmlXPathFreeContext (context);

  return result;
}

static void
its_rule_list_extract_text (its_rule_list_ty *rules,
                            xmlNode *node,
                            const char *logical_filename,
                            flag_context_list_table_ty *flag_table,
                            message_list_ty *mlp)
{
  if (node->type == XML_ELEMENT_NODE)
    {
      struct its_value_list_ty *values;
      xmlNode *n;
      const char *value;
      char *comment = NULL;

      values = its_rule_list_eval (rules, node);

      value = its_value_list_get_value (values, "locNote");
      if (value)
        comment = xstrdup (value);
      else
        {
          value = its_value_list_get_value (values, "locNotePointer");
          if (value)
            comment = _its_get_content (node, value);
        }

      for (n = node->children; n; n = n->next)
        if (n->type == XML_TEXT_NODE)
          {
            xmlChar *content = xmlNodeGetContent (n);

            if (xmlStrlen (content) > 0)
              {
                lex_pos_ty pos;

                pos.file_name = xstrdup (logical_filename);
                pos.line_number = xmlGetLineNo (n);

                remember_a_message (mlp, NULL,
                                    xstrdup ((const char *) content),
                                    null_context, &pos,
                                    comment, NULL);
                free (comment);
              }
            xmlFree (content);
          }
    }
}

void
its_rule_list_extract (its_rule_list_ty *rules,
                       FILE *fp, const char *real_filename,
                       const char *logical_filename,
                       flag_context_list_table_ty *flag_table,
                       msgdomain_list_ty *mdlp)
{
  const struct its_value_ty values[] =
    {
      { "translate", "yes" },
      { NULL, NULL }
    };
  xmlDoc *doc;
  struct its_node_list_ty nodes;
  size_t i;

  doc = xmlReadFd (fileno (fp), logical_filename, "utf-8",
                   XML_PARSE_NONET
                   | XML_PARSE_NOWARNING
                   | XML_PARSE_NOBLANKS
                   | XML_PARSE_NOERROR);
  if (doc == NULL)
    return;

  its_rule_list_apply (rules, doc);

  memset (&nodes, 0, sizeof (struct its_node_list_ty));
  its_rule_list_extract_nodes (rules,
                               &nodes,
                               xmlDocGetRootElement (doc),
                               values);

  for (i = 0; i < nodes.nitems; i++)
    its_rule_list_extract_text (rules, nodes.items[i],
                                logical_filename,
                                flag_table,
                                mdlp->item[0]->messages);

  free (nodes.items);
  xmlFreeDoc (doc);
}