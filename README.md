### MicroRegExp : A further development of SLRE regexp engine

MicroRegExp — A lightweight regular expression library for C for microcontrollers.
Compiles and runs under Windows/Cygwin, Linux and FreeRTOS (ESP32)

A patched and extended fork of SLRE, originally based on the work
of Sergey Lyubka and subsequently maintained by Aquefir Consulting.
(https://github.com/aquefir/slre)


### API:

The library provides three APIs, all available for direct use:
 
  1. PHP preg-style. API A well-known preg_* interface inspired by PHP's regular expression functions.
  2. PCRE-like API. An additional API with pl_* function and structure names
  3. Native SLRE API The original low-level matching interface - slre_match()
 
  The PHP preg-style API is the primary high-level interface.

### Documentation?:

Uhm.. there are preg_test.c file which can be used as a reference

###LIMITATIONS:
 
  * Lookarounds are not supported.
  * Matching operates on bytes: UTF-8 character ranges such as [à-ÿ] are not supported.
