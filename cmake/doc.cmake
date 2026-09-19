# Build the small .NET Framework helper with the Windows framework compiler.
# No modern .NET runtime is distributed or required by the application.
FetchContent_Declare(docbinary URL https://api.nuget.org/v3-flatcontainer/docsharp.binary.doc/0.21.0/docsharp.binary.doc.0.21.0.nupkg
  URL_HASH SHA256=438a0b5da6ae1d98e53dcbda5505cbf0ca1eb19a917b96d101c0c9d3b0fa1f2e)
FetchContent_Declare(doccommon URL https://api.nuget.org/v3-flatcontainer/docsharp.binary.common/0.21.0/docsharp.binary.common.0.21.0.nupkg
  URL_HASH SHA256=e7e8251def04befa6b3f1cb480dadc5eb33b0395c5db6738a8b0eca84a276f68)
FetchContent_Declare(doccompression URL https://api.nuget.org/v3-flatcontainer/system.io.compression/4.3.0/system.io.compression.4.3.0.nupkg
  URL_HASH SHA256=7f93eb4254208f95e3d999c7c575bc5e23a2bda06f7ea0daa3d49be805629f20)
FetchContent_MakeAvailable(docbinary doccommon doccompression)
set(DOC_COMPILER "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319/csc.exe")
if(NOT EXISTS "${DOC_COMPILER}")
  message(FATAL_ERROR "The .NET Framework 4.8 C# compiler is required for DOC conversion")
endif()
set(DOC_LIBRARIES
  "${docbinary_SOURCE_DIR}/lib/net462/DocSharp.Binary.Doc.dll"
  "${doccommon_SOURCE_DIR}/lib/net462/DocSharp.Binary.Common.dll"
  "${doccompression_SOURCE_DIR}/lib/net46/System.IO.Compression.dll")
set(DOC_REFERENCES)
foreach(library IN LISTS DOC_LIBRARIES)
  file(TO_NATIVE_PATH "${library}" native_library)
  list(APPEND DOC_REFERENCES "/reference:${native_library}")
endforeach()
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/doc_helper.cs" DOC_SOURCE)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VolturaBooksDoc.exe"
  COMMAND "${DOC_COMPILER}" /nologo /target:winexe /platform:x64 /optimize+
    "/out:${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VolturaBooksDoc.exe"
    /reference:System.Drawing.dll ${DOC_REFERENCES} "${DOC_SOURCE}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DOC_LIBRARIES} "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
  DEPENDS src/doc_helper.cs ${DOC_LIBRARIES} VERBATIM)
add_custom_target(VolturaBooksDoc ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VolturaBooksDoc.exe")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VolturaBooksDoc.exe" ${DOC_LIBRARIES} DESTINATION .)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/doc-third-party-notices.txt" DOC_NOTICES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/doc-third-party-notices.txt)
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" "\n${DOC_NOTICES}\n")
if(BOOKS_BUILD_TESTS)
  # Upstream MIT-licensed format fixture; test data only, never installed.
  set(DOC_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/doc-fixture.doc")
  if(NOT EXISTS "${DOC_FIXTURE}")
    file(DOWNLOAD "https://raw.githubusercontent.com/manfromarce/DocSharp/7c996f8f6934139d4a4a698963baeb59fd8d60df/test-files/DOC/Word%2097-2003/DOC.doc"
      "${DOC_FIXTURE}" EXPECTED_HASH SHA256=53ab718412fe7df64e72f7f8245396f944815c2c467c32a3248ea6596f0541ea TLS_VERIFY ON)
  endif()
  file(SHA256 "${DOC_FIXTURE}" DOC_FIXTURE_HASH)
  if(NOT DOC_FIXTURE_HASH STREQUAL "53ab718412fe7df64e72f7f8245396f944815c2c467c32a3248ea6596f0541ea")
    message(FATAL_ERROR "DOC test fixture checksum mismatch")
  endif()
  add_executable(doc_process_fixture tests/doc_process_fixture.cpp)
  target_compile_definitions(doc_process_fixture PRIVATE UNICODE _UNICODE)
  add_executable(doc_tests tests/doc_tests.cpp src/docx_text.cpp)
  target_link_libraries(doc_tests PRIVATE books_cover)
  target_compile_options(doc_tests PRIVATE /utf-8)
  add_dependencies(doc_tests VolturaBooksDoc doc_process_fixture)
  add_executable(reader_loading_tests tests/reader_loading_tests.cpp)
  target_include_directories(reader_loading_tests PRIVATE src)
  target_compile_options(reader_loading_tests PRIVATE /utf-8)
  add_test(NAME reader_loading COMMAND reader_loading_tests)
  add_test(NAME doc_conversion COMMAND doc_tests "${DOC_FIXTURE}")
  set_tests_properties(doc_conversion PROPERTIES TIMEOUT 45)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME doc_format COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/doc_fixture.py"
      "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VolturaBooksDoc.exe" "${DOC_FIXTURE}")
    set_tests_properties(doc_format PROPERTIES TIMEOUT 45)
  endif()
endif()
