add_library(analyzer STATIC
    analyzer.cpp
)

target_include_directories(analyzer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(analyzer PUBLIC sql_parser storage catalog)

target_link_libraries(analyzer PRIVATE log common)
