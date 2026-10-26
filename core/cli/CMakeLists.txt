add_executable(micnote-cli main.cpp)
target_link_libraries(micnote-cli PRIVATE micnote::micnote micnote::warnings)
