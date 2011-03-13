FIND_PATH(LPSOLVE_INCLUDE_DIR lp_solve/lp_lib.hpp)

FIND_LIBRARY(LPSOLVE_LIBRARY NAMES lpsolve)

IF (LPSOLVE_INCLUDE_DIR AND LPSOLVE_LIBRARY)
  SET(LPSOLVE_FOUND TRUE)
ENDIF (LPSOLVE_INCLUDE_DIR AND LPSOLVE_LIBRARY)


IF (LPSOLVE_FOUND)
  IF (NOT LpSolve_FIND_QUIETLY)
    MESSAGE(STATUS "Found LpSolve: ${LPSOLVE_LIBRARY}")
  ENDIF (NOT LpSolve_FIND_QUIETLY)
ELSE (LPSOLVE_FOUND)
  IF (LpSolve_FIND_REQUIRED)
    MESSAGE(FATAL_ERROR "Could not find LpSolve")
  ENDIF (LpSolve_FIND_REQUIRED)
ENDIF (LPSOLVE_FOUND)

