idf_component_register(
    SRCS "jobs_model.c" "jobs_parse.c" "jobs_validate.c" "jobs_serialize.c"
         "jobs_platform.c" "jobs_runtime.c" "jobs_schedule.c" "jobs_store.c" "jobs_build.c"
    INCLUDE_DIRS "."
    REQUIRES devos_actions devos_json devos_events
    PRIV_REQUIRES devos_config
)
