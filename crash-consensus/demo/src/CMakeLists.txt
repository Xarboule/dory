add_executable(main-st main-st.cpp)
target_link_libraries(main-st ${CRASH_CONSENSUS})

add_executable(main-st-lat main-st-lat.cpp)
target_link_libraries(main-st-lat ${CRASH_CONSENSUS})

add_executable(main-dt main-dt.cpp)
target_link_libraries(main-dt ${CRASH_CONSENSUS})

add_executable(main-st-lat-tof main-st-lat-tof.cpp)
target_link_libraries(main-st-lat-tof ${CRASH_CONSENSUS})

add_executable(main-st-lat-tof-scale main-st-lat-tof-scale.cpp)
target_link_libraries(main-st-lat-tof-scale ${CRASH_CONSENSUS})

add_executable(stats stats.cpp)
add_executable(fifo fifo.cpp)
