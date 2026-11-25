// The C of the module `xml` (devices/xml.ent): documents read and XPath
// run by libxml2 (https://gitlab.gnome.org/GNOME/libxml2, MIT licence).
// Its functions are declared here, as its headers have them (their
// folder, include/libxml2, is not one the compiler looks in by itself);
// of its structures only the first fields, which have not changed since
// libxml2 2.0, are read.
#include "ent_extern.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#define ENT_XML_SAME_RESULT __attribute__((annotate("ent.same_result")))
#else
#define ENT_XML_SAME_RESULT
#endif

typedef unsigned char xmlChar;
typedef struct xml_doc xml_doc;
typedef struct xml_context xml_context;
typedef struct xml_node xml_node;
typedef struct {
  int nodeNr;
  int nodeMax;
  xml_node **nodeTab;
} xml_node_set;
typedef struct {
  int type; // 1 nodes, 2 boolean, 3 number, 4 string
  xml_node_set *nodesetval;
} xml_object;

extern xml_doc *htmlReadMemory(const char *buffer, int size, const char *url, const char *encoding, int options);
extern xml_doc *xmlReadMemory(const char *buffer, int size, const char *url, const char *encoding, int options);
extern void xmlFreeDoc(xml_doc *doc);
extern xml_context *xmlXPathNewContext(xml_doc *doc);
extern void xmlXPathFreeContext(xml_context *context);
extern xml_object *xmlXPathEvalExpression(const xmlChar *expression, xml_context *context);
extern void xmlXPathFreeObject(xml_object *object);
extern xmlChar *xmlXPathCastToString(xml_object *object);
extern xmlChar *xmlXPathCastNodeToString(xml_node *node);
extern void xmlSetGenericErrorFunc(void *context, void (*handler)(void *, const char *, ...));
extern void xmlSetStructuredErrorFunc(void *context, void (*handler)(void *, void *));
extern void (*xmlFree)(void *);

enum { XML_RECOVER = 1, XML_NOERROR = 32, XML_NOWARNING = 64, XML_NONET = 2048 };

static void said_nothing(void *context, const char *message, ...) { (void)context, (void)message; }
static void said_nothing_structured(void *context, void *error) { (void)context, (void)error; }

// A document read, and an expression run on it; `found` NULL where
// either is wrong.
typedef struct {
  xml_doc *doc;
  xml_context *context;
  xml_object *found;
} xml_run;

static xml_run run_xpath(const ent_text *document, bool html, const ent_text *expression) {
  static bool quiet;
  if (!quiet) {
    xmlSetGenericErrorFunc(NULL, said_nothing);
    xmlSetStructuredErrorFunc(NULL, said_nothing_structured);
    quiet = true;
  }
  xml_run r = {0};
  int size = document ? (int)document->length : 0;
  const char *bytes = document && size ? (const char *)document->bytes : "";
  int options = XML_NOERROR | XML_NOWARNING | XML_NONET;
  r.doc = html ? htmlReadMemory(bytes, size, NULL, NULL, options | XML_RECOVER)
               : xmlReadMemory(bytes, size, NULL, NULL, options);
  if (!r.doc)
    return r;
  r.context = xmlXPathNewContext(r.doc);
  size_t n = expression ? expression->length : 0;
  char *said = malloc(n + 1);
  if (r.context && said) {
    if (n)
      memcpy(said, expression->bytes, n);
    said[n] = 0;
    r.found = xmlXPathEvalExpression((const xmlChar *)said, r.context);
  }
  free(said);
  return r;
}
static void run_done(xml_run *r) {
  if (r->found)
    xmlXPathFreeObject(r->found);
  if (r->context)
    xmlXPathFreeContext(r->context);
  if (r->doc)
    xmlFreeDoc(r->doc);
}
static const ent_text *text_of(xmlChar *s) {
  size_t n = s ? strlen((const char *)s) : 0;
  ent_text *t = ent_text_new((uint32_t)n);
  if (t && n)
    memcpy(t->bytes, s, n);
  if (s)
    xmlFree(s);
  return t;
}

ENT_XML_SAME_RESULT const ent_text *ent_xml_xpath_kind(const ent_text *document, bool html,
                                                      const ent_text *expression) {
  xml_run r = run_xpath(document, html, expression);
  int type = r.found ? r.found->type : 0;
  run_done(&r);
  const char *said = type == 1 ? "nodes" : type == 2 ? "boolean" : type == 3 ? "number" : type == 4 ? "string" : "";
  size_t n = strlen(said);
  ent_text *t = ent_text_new((uint32_t)n);
  if (t && n)
    memcpy(t->bytes, said, n);
  return t;
}

ENT_XML_SAME_RESULT int32_t ent_xml_xpath_count(const ent_text *document, bool html, const ent_text *expression) {
  xml_run r = run_xpath(document, html, expression);
  int32_t count = !r.found || r.found->type != 1 ? -1 : r.found->nodesetval ? r.found->nodesetval->nodeNr : 0;
  run_done(&r);
  return count;
}

ENT_XML_SAME_RESULT const ent_text *ent_xml_xpath_text(const ent_text *document, bool html,
                                                      const ent_text *expression) {
  xml_run r = run_xpath(document, html, expression);
  const ent_text *t = r.found ? text_of(xmlXPathCastToString(r.found)) : ent_text_new(0);
  run_done(&r);
  return t;
}

ENT_XML_SAME_RESULT const ent_text *ent_xml_xpath_item(const ent_text *document, bool html,
                                                      const ent_text *expression, int32_t k) {
  xml_run r = run_xpath(document, html, expression);
  bool there = r.found && r.found->type == 1 && r.found->nodesetval && k >= 0 && k < r.found->nodesetval->nodeNr;
  const ent_text *t = there ? text_of(xmlXPathCastNodeToString(r.found->nodesetval->nodeTab[k])) : ent_text_new(0);
  run_done(&r);
  return t;
}
