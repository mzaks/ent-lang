configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

find_program(ENT_LIT lit
  HINTS ${PROJECT_SOURCE_DIR}/.venv/bin
  REQUIRED)

add_custom_target(check-ent
  COMMAND ${ENT_LIT} -sv ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ent-opt ent-translate
  USES_TERMINAL)
