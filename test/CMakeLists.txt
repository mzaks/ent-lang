configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

find_program(ECS_LIT lit
  HINTS ${PROJECT_SOURCE_DIR}/.venv/bin
  REQUIRED)

add_custom_target(check-ecs
  COMMAND ${ECS_LIT} -sv ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ecs-opt ecs-translate
  USES_TERMINAL)
