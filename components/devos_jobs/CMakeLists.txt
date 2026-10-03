idf_component_register(
    SRCS "jobs_model.c" "jobs_parse.c" "jobs_validate.c" "jobs_serialize.c"
    INCLUDE_DIRS "."
    REQUIRES devos_actions
)
