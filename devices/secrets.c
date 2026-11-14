// The secrets device (devices/secrets.ent): texts kept in the system's
// keyring. Linux: libsecret, loaded with dlopen when first needed (so a
// program does not need it to start, and builds without its headers:
// the few types it takes are written out below, as libsecret 0.20 has
// them). Windows: the Credential Manager. macOS: the login keychain
// (generic passwords, by the Security framework). Elsewhere: none.

#include "ent_extern.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A text as a C string (none: NULL, the caller frees it).
static char *ent_secrets_string(const ent_text *t) {
  uint32_t n = t ? t->length : 0;
  if (n && memchr(t->bytes, 0, n))
    return NULL;
  char *s = malloc((size_t)n + 1);
  if (!s)
    return NULL;
  if (n)
    memcpy(s, t->bytes, n);
  s[n] = 0;
  return s;
}

static const ent_text *ent_secrets_text(const char *bytes, size_t length) {
  if (!bytes || !length || length > UINT32_MAX)
    return 0;
  ent_text *made = ent_text_new((uint32_t)length);
  memcpy(made->bytes, bytes, length);
  return made;
}

#if defined(_WIN32)
#include <windows.h>
#include <wincred.h>

// "service/account", the name the credential has.
static char *ent_secrets_target(const ent_text *service,
                                const ent_text *account) {
  char *s = ent_secrets_string(service), *a = ent_secrets_string(account);
  char *target = NULL;
  if (s && a && (target = malloc(strlen(s) + strlen(a) + 2)))
    strcat(strcat(strcpy(target, s), "/"), a);
  free(s);
  free(a);
  return target;
}

static bool ent_secrets_os_available(void) { return true; }

static bool ent_secrets_os_put(const ent_text *service,
                               const ent_text *account,
                               const ent_text *value) {
  char *target = ent_secrets_target(service, account);
  if (!target)
    return false;
  CREDENTIALA c;
  memset(&c, 0, sizeof c);
  c.Type = CRED_TYPE_GENERIC;
  c.TargetName = target;
  c.CredentialBlobSize = value ? value->length : 0;
  c.CredentialBlob = value ? (LPBYTE)value->bytes : NULL;
  c.Persist = CRED_PERSIST_LOCAL_MACHINE;
  BOOL kept = CredWriteA(&c, 0);
  free(target);
  return kept;
}

static const ent_text *ent_secrets_os_get(const ent_text *service,
                                          const ent_text *account) {
  char *target = ent_secrets_target(service, account);
  if (!target)
    return 0;
  PCREDENTIALA c = NULL;
  const ent_text *found = 0;
  if (CredReadA(target, CRED_TYPE_GENERIC, 0, &c)) {
    found = ent_secrets_text((const char *)c->CredentialBlob,
                             c->CredentialBlobSize);
    CredFree(c);
  }
  free(target);
  return found;
}

static bool ent_secrets_os_forget(const ent_text *service,
                                  const ent_text *account) {
  char *target = ent_secrets_target(service, account);
  if (!target)
    return false;
  BOOL gone = CredDeleteA(target, CRED_TYPE_GENERIC, 0) ||
              GetLastError() == ERROR_NOT_FOUND;
  free(target);
  return gone;
}

#elif defined(__linux__)
#include <dlfcn.h>

// libsecret's and GLib's types, as far as they are used.
typedef struct {
  const char *name;
  int type; // SECRET_SCHEMA_ATTRIBUTE_STRING: 0
} ent_secret_attribute;
typedef struct {
  const char *name;
  int flags; // SECRET_SCHEMA_NONE: 0
  ent_secret_attribute attributes[32];
  int reserved;
  void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5,
      *reserved6, *reserved7;
} ent_secret_schema;
typedef struct {
  uint32_t domain;
  int code;
  char *message;
} ent_g_error;

static const ent_secret_schema ent_secrets_schema = {
    "org.ent-lang.Secret", 0, {{"service", 0}, {"account", 0}, {NULL, 0}}};

static int (*store)(const ent_secret_schema *, const char *collection,
                    const char *label, const char *password, void *cancel,
                    ent_g_error **error, ...);
static char *(*lookup)(const ent_secret_schema *, void *cancel,
                       ent_g_error **error, ...);
static int (*clear)(const ent_secret_schema *, void *cancel,
                    ent_g_error **error, ...);
static void (*password_free)(char *);
static void (*error_free)(ent_g_error *);
// 0: not looked for yet, 1: there, -1: not there.
static int ent_secrets_loaded;

static bool ent_secrets_load(void) {
  if (ent_secrets_loaded == 0) {
    ent_secrets_loaded = -1;
    void *lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (lib) {
      *(void **)&store = dlsym(lib, "secret_password_store_sync");
      *(void **)&lookup = dlsym(lib, "secret_password_lookup_sync");
      *(void **)&clear = dlsym(lib, "secret_password_clear_sync");
      *(void **)&password_free = dlsym(lib, "secret_password_free");
      *(void **)&error_free = dlsym(lib, "g_error_free");
      if (store && lookup && clear && password_free && error_free)
        ent_secrets_loaded = 1;
    }
  }
  return ent_secrets_loaded == 1;
}

// Whether a call went wrong (its error let go).
static bool ent_secrets_failed(ent_g_error *error) {
  if (!error)
    return false;
  error_free(error);
  return true;
}

// (Available: libsecret is there and a keyring answers a lookup.)
static int ent_secrets_answers;
static bool ent_secrets_os_available(void) {
  if (!ent_secrets_load())
    return false;
  if (ent_secrets_answers == 0) {
    ent_g_error *error = NULL;
    char *found = lookup(&ent_secrets_schema, NULL, &error, "service",
                         "org.ent-lang.none", "account", "", (char *)NULL);
    if (found)
      password_free(found);
    ent_secrets_answers = ent_secrets_failed(error) ? -1 : 1;
  }
  return ent_secrets_answers == 1;
}

static bool ent_secrets_os_put(const ent_text *service,
                               const ent_text *account,
                               const ent_text *value) {
  if (!ent_secrets_load())
    return false;
  char *s = ent_secrets_string(service), *a = ent_secrets_string(account),
       *v = ent_secrets_string(value), *label = NULL;
  bool kept = false;
  if (s && a && v && (label = malloc(strlen(s) + strlen(a) + 4))) {
    strcat(strcat(strcpy(label, s), ": "), a);
    ent_g_error *error = NULL;
    int done = store(&ent_secrets_schema, NULL, label, v, NULL, &error,
                     "service", s, "account", a, (char *)NULL);
    kept = !ent_secrets_failed(error) && done;
  }
  if (v)
    memset(v, 0, strlen(v));
  free(s);
  free(a);
  free(v);
  free(label);
  return kept;
}

static const ent_text *ent_secrets_os_get(const ent_text *service,
                                          const ent_text *account) {
  if (!ent_secrets_load())
    return 0;
  char *s = ent_secrets_string(service), *a = ent_secrets_string(account);
  const ent_text *found = 0;
  if (s && a) {
    ent_g_error *error = NULL;
    char *value = lookup(&ent_secrets_schema, NULL, &error, "service", s,
                         "account", a, (char *)NULL);
    if (!ent_secrets_failed(error) && value)
      found = ent_secrets_text(value, strlen(value));
    if (value)
      password_free(value);
  }
  free(s);
  free(a);
  return found;
}

static bool ent_secrets_os_forget(const ent_text *service,
                                  const ent_text *account) {
  if (!ent_secrets_load())
    return false;
  char *s = ent_secrets_string(service), *a = ent_secrets_string(account);
  bool gone = false;
  if (s && a) {
    ent_g_error *error = NULL;
    clear(&ent_secrets_schema, NULL, &error, "service", s, "account", a,
          (char *)NULL);
    gone = !ent_secrets_failed(error);
  }
  free(s);
  free(a);
  return gone;
}

#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

// The frameworks are asked for here, not in a .link (which every
// system's build would read): every Mac has them.
__asm__(".linker_option \"-framework\", \"Security\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"");

static CFStringRef ent_secrets_cf(const ent_text *t) {
  if (t && t->length && memchr(t->bytes, 0, t->length))
    return NULL;
  return CFStringCreateWithBytes(NULL, t ? (const UInt8 *)t->bytes : NULL,
                                 t ? t->length : 0, kCFStringEncodingUTF8,
                                 false);
}

// A generic password of the login keychain, found by its service and
// account; none where either is not a text of UTF-8 without a 0.
static CFMutableDictionaryRef ent_secrets_query(const ent_text *service,
                                                const ent_text *account) {
  CFStringRef s = ent_secrets_cf(service), a = ent_secrets_cf(account);
  CFMutableDictionaryRef query = NULL;
  if (s && a &&
      (query = CFDictionaryCreateMutable(NULL, 0,
                                         &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks))) {
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, s);
    CFDictionarySetValue(query, kSecAttrAccount, a);
  }
  if (s)
    CFRelease(s);
  if (a)
    CFRelease(a);
  return query;
}

static bool ent_secrets_os_available(void) { return true; }

static bool ent_secrets_os_put(const ent_text *service,
                               const ent_text *account,
                               const ent_text *value) {
  CFMutableDictionaryRef query = ent_secrets_query(service, account);
  if (!query)
    return false;
  CFDataRef data = CFDataCreate(
      NULL, value ? (const UInt8 *)value->bytes : NULL, value ? value->length : 0);
  OSStatus status = errSecParam;
  if (data) {
    // Kept again where it is there (its access as it was), else added.
    CFDictionaryRef change = CFDictionaryCreate(
        NULL, (const void **)&kSecValueData, (const void **)&data, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    status = change ? SecItemUpdate(query, change) : errSecAllocate;
    if (change)
      CFRelease(change);
    if (status == errSecItemNotFound) {
      CFDictionarySetValue(query, kSecValueData, data);
      status = SecItemAdd(query, NULL);
    }
    CFRelease(data);
  }
  CFRelease(query);
  return status == errSecSuccess;
}

static const ent_text *ent_secrets_os_get(const ent_text *service,
                                          const ent_text *account) {
  CFMutableDictionaryRef query = ent_secrets_query(service, account);
  if (!query)
    return 0;
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef data = NULL;
  const ent_text *found = 0;
  if (SecItemCopyMatching(query, &data) == errSecSuccess && data &&
      CFGetTypeID(data) == CFDataGetTypeID())
    found = ent_secrets_text((const char *)CFDataGetBytePtr(data),
                             (size_t)CFDataGetLength(data));
  if (data)
    CFRelease(data);
  CFRelease(query);
  return found;
}

static bool ent_secrets_os_forget(const ent_text *service,
                                  const ent_text *account) {
  CFMutableDictionaryRef query = ent_secrets_query(service, account);
  if (!query)
    return false;
  OSStatus status = SecItemDelete(query);
  CFRelease(query);
  return status == errSecSuccess || status == errSecItemNotFound;
}

#else
static bool ent_secrets_os_available(void) { return false; }
static bool ent_secrets_os_put(const ent_text *service,
                               const ent_text *account,
                               const ent_text *value) {
  (void)service, (void)account, (void)value;
  return false;
}
static const ent_text *ent_secrets_os_get(const ent_text *service,
                                          const ent_text *account) {
  (void)service, (void)account;
  return 0;
}
static bool ent_secrets_os_forget(const ent_text *service,
                                  const ent_text *account) {
  (void)service, (void)account;
  return false;
}
#endif

// ENT_SECRETS=none: no keyring, as if there were none, on every system
// (for tests, which should neither read nor write the user's keyring).
static bool ent_secrets_none(void) {
  const char *said = getenv("ENT_SECRETS");
  return said && strcmp(said, "none") == 0;
}

bool ent_secrets_secrets_available(void) {
  return !ent_secrets_none() && ent_secrets_os_available();
}

bool ent_secrets_secret_put(const ent_text *service, const ent_text *account,
                            const ent_text *value) {
  return !ent_secrets_none() && ent_secrets_os_put(service, account, value);
}

const ent_text *ent_secrets_secret_get(const ent_text *service,
                                       const ent_text *account) {
  return ent_secrets_none() ? 0 : ent_secrets_os_get(service, account);
}

bool ent_secrets_secret_forget(const ent_text *service,
                               const ent_text *account) {
  return !ent_secrets_none() && ent_secrets_os_forget(service, account);
}
