@ echo off
IF NOT DEFINED PYTHON_EXECUTABLE SET PYTHON_EXECUTABLE=python
"%PYTHON_EXECUTABLE%" "%~dp0\mock-distcc" %*
