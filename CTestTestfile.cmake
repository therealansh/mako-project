# CMake generated Testfile for 
# Source directory: /home/anshtyagi.linux/mako_ansh/mako-project
# Build directory: /home/anshtyagi.linux/mako_ansh/mako-project
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(test_marshal "/home/anshtyagi.linux/mako_ansh/mako-project/test_marshal")
set_tests_properties(test_marshal PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;892;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_rpc "/home/anshtyagi.linux/mako_ansh/mako-project/test_rpc")
set_tests_properties(test_rpc PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;899;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_future "/home/anshtyagi.linux/mako_ansh/mako-project/test_future")
set_tests_properties(test_future PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;913;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_reactor "/home/anshtyagi.linux/mako_ansh/mako-project/test_reactor")
set_tests_properties(test_reactor PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;920;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_coroutine "/home/anshtyagi.linux/mako_ansh/mako-project/test_coroutine")
set_tests_properties(test_coroutine PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;928;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_rpc_extended "/home/anshtyagi.linux/mako_ansh/mako-project/test_rpc_extended")
set_tests_properties(test_rpc_extended PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;936;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_reactor_extended "/home/anshtyagi.linux/mako_ansh/mako-project/test_reactor_extended")
set_tests_properties(test_reactor_extended PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;943;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_timeout_race "/home/anshtyagi.linux/mako_ansh/mako-project/test_timeout_race")
set_tests_properties(test_timeout_race PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;950;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(test_and_event "/home/anshtyagi.linux/mako_ansh/mako-project/test_and_event")
set_tests_properties(test_and_event PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;957;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(kdv_format_test "/home/anshtyagi.linux/mako_ansh/mako-project/kdv_format_test")
set_tests_properties(kdv_format_test PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;964;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(simpleTransaction "/home/anshtyagi.linux/mako_ansh/mako-project/simpleTransaction")
set_tests_properties(simpleTransaction PROPERTIES  _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;982;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(simplePaxos "bash" "-c" "bash ./src/mako/update_config.sh && bash ./examples/simplePaxos.sh")
set_tests_properties(simplePaxos PROPERTIES  WORKING_DIRECTORY "/home/anshtyagi.linux/mako_ansh/mako-project" _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;985;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
add_test(shard1ReplicationSimple "bash" "-c" "bash ./examples/test_1shard_replication_simple.sh")
set_tests_properties(shard1ReplicationSimple PROPERTIES  WORKING_DIRECTORY "/home/anshtyagi.linux/mako_ansh/mako-project" _BACKTRACE_TRIPLES "/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;991;add_test;/home/anshtyagi.linux/mako_ansh/mako-project/CMakeLists.txt;0;")
subdirs("third-party/erpc")
