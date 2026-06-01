/*******************************************************************
 * Copyright (C) Huawei Technologies, 2019
 *
 * Dump Mem header file
 *******************************************************************/

#ifndef __DUMPMEM_H__
#define __DUMPMEM_H__

#ifdef ENABLE_ODD_MEMDUMP
#include <type_traits>

#include <array>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fstream>
#include <iostream>
#include <sstream>

#include <boost/serialization/access.hpp>
#include <boost/serialization/array.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/serialization/complex.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/serialization/vector.hpp>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

// TODO(krm)
// DUMP_MEM_FILE_DIR should be configurable, be visible to:
// SAL-SQL and SliceServer
#define DUMP_MEM_FILE_DIR "/tmp/"
#define DUMP_MEM_FNAME DUMP_MEM_FILE_DIR "mem.data"
#define DUMP_MET_FNAME DUMP_MEM_FILE_DIR "mem.meta"

namespace Huawei {
namespace Common {

/////////////////////////////////////////////////////////////////////
// Developer's Guide:                                              //
/////////////////////////////////////////////////////////////////////
// in .h:
// Define your own Macro with the list of members you want to expose.
// e.g. define {DUMPPONT_NAME} (_def)
//   _def (member_varibale_1, SIMPLE)
//   _def (member_function_1, CALL_SIMPLE)
// For member functions use CALL_SIMPLE, for non variables use SIMPLE
//
// Then pass the name of the class and the dumppoint to ODD_INIT, i.e.,
// use: ODD_INIT(<CLASSNAME>, <DUMPPOINT_NAME>).
// Notes:
//   1) Pass class name as is, and not as string
//   2) The macro uses public: keyword, so either use it as the last
//      line in the header file, or be cautious to return to private:
//      after using if needed.
/////////////////////////////////////////////////////////////////////
// in .cc:
// In the class constructor pass class name and unique id to
// ODD_INIT_CC(<CLASSNAME>, <UNIQUE_ID>)
//
// in {sal,slice}odddecoder.cc:
// Add your class name to the Macro TAURUS_ON_DEMAND_DEBUG_CLASS_LIST
// e.g., _def(<CLASSNAME>)
//
/////////////////////////////////////////////////////////////////////
// Types used                                                      //
/////////////////////////////////////////////////////////////////////
typedef void *DumpMemHandle;

// DumpMemArchive types
typedef boost::archive::binary_oarchive DumpMemoArchive;
typedef boost::archive::binary_iarchive DumpMemiArchive;

// DumpMemCB (used by perf publisher)
typedef char *(*DumpMemCB)(DumpMemHandle, DumpMemoArchive &);
// map key -> CB (used by perf publisher)
typedef std::unordered_map<std::string, DumpMemCB> DumpMemCBMap;
// map key -> DumpMemHandle (used by perf publisher)
typedef std::unordered_map<std::string, DumpMemHandle> DumpMemHandleMap;
// map key -> Typea (as string) (used by perf publisher)
typedef std::unordered_map<std::string, std::string> DumpMemTypeMap;

// dumpmem file type code (used during encode/decode)
enum dumpMemFileTypeCode {
  // Purpose is to start files with a char code to be able to identify file
  // to avoid mixup and produce meanigful errors when decoding files.
  datafile = '$',
  metafile = '#'
};

// In the future, we might need to print each dumppoint in a single line.
#define STL_PRINT_ENDL std::endl

///////////////////////////////////////////////////////////////////
// stl_to_string templates                                       //
///////////////////////////////////////////////////////////////////
// Different function/templates to printout different types.
// Used during decoding

// A template for simple numeric
template <typename T, typename = typename std::enable_if<
                          std::is_arithmetic<T>::value, T>::type>
std::string stl_to_string(const T &defaulto) {
  std::stringstream ss;
  ss << defaulto;
  return ss.str();
}
// Forward declaration of print templates, since they can reference
// each other depending on the stl container combination.
std::string stl_to_string(const char &defaulto);
std::string stl_to_string(const std::string &defaulto);
template <typename T>
std::string stl_to_string(const std::atomic<T> &defaulto);

template <class T, class U>
std::string stl_to_string(const std::pair<U, T> &apair);
template <class T>
std::string stl_to_string(const std::set<T> &aset);
template <class T, class U>
std::string stl_to_string(const std::unordered_map<U, T> &amap);
template <class T, class U>
std::string stl_to_string(const std::map<T, U> &amap);
template <class T>
std::string stl_to_string(const std::vector<T> &avec);
template <class T>
std::string stl_to_string(const std::list<T> &alist);
template <class T, std::size_t N>
std::string stl_to_string(const std::array<T, N> &anarray);

// A template for atomic types
template <typename T>
std::string stl_to_string(const std::atomic<T> &defaulto) {
  return stl_to_string(defaulto.load());
}
// A template for simple std::pair
template <class T, class U>
std::string stl_to_string(const std::pair<U, T> &apair) {
  std::stringstream ss;
  ss << "(" << stl_to_string(apair.first) << "," << stl_to_string(apair.second)
     << ")";
  return ss.str();
}
// A template for simple std::set
template <class T>
std::string stl_to_string(const std::set<T> &aset) {
  std::stringstream ss;
  ss << "size:" << aset.size() << "[";
  for (auto const &it : aset) {
    ss << stl_to_string(it) << ";";
  }
  ss << "]" << STL_PRINT_ENDL;
  return ss.str();
}
// A template for simple std::unordered_map
template <class T, class U>
std::string stl_to_string(const std::unordered_map<U, T> &amap) {
  std::stringstream ss;
  ss << "size:" << amap.size() << "[";
  for (auto const &it : amap) {
    ss << stl_to_string(it);
  }
  ss << "]" << STL_PRINT_ENDL;
  return ss.str();
}
// A template for simple std::map
template <class T, class U>
std::string stl_to_string(const std::map<T, U> &amap) {
  std::stringstream ss;
  ss << "size:" << amap.size() << "[";
  for (auto const &it : amap) {
    ss << stl_to_string(it);
  }
  ss << "]" << STL_PRINT_ENDL;
  return ss.str();
}
// A template for simple std::vector
template <class T>
std::string stl_to_string(const std::vector<T> &avec) {
  std::stringstream ss;
  ss << "size:" << avec.size() << "[";
  for (auto const &it : avec) {
    ss << stl_to_string(it) << ";";
  }
  ss << "]" << STL_PRINT_ENDL;
  return ss.str();
}
// A template for simple std::list
template <class T>
std::string stl_to_string(const std::list<T> &alist) {
  std::stringstream ss;
  ss << "size:" << alist.size() << "[";
  for (auto const &it : alist) {
    ss << stl_to_string(it) << ";";
  }
  ss << "]" << STL_PRINT_ENDL;
  return ss.str();
}
// A template for simple std::array
template <class T, std::size_t N>
std::string stl_to_string(const std::array<T, N> &anarray) {
  std::stringstream ss;
  ss << "size:" << anarray.size() << "[";
  for (auto const &it : anarray) {
    ss << stl_to_string(it) << ";";
  }
  ss << "]" << STL_PRINT_ENDL;
  return ss.str();
}

///////////////////////////////////////////////////////////////////
// serialize/deserialize templates                               //
///////////////////////////////////////////////////////////////////
// The purpose of these templates is to separate all data types
// from atomic data types which are not supported by default by
// boost::serialization.
//
template <typename T, typename Archive>
void __deserialize_var_(std::atomic<T> &var, Archive &iar) {
  T temp;
  iar >> temp;
  var = temp;
}

template <typename T, typename Archive>
void __deserialize_var_(T &var, Archive &iar) {
  iar >> var;
}

template <typename T, typename Archive>
void __serialize_var_(const std::atomic<T> &var, Archive &oar) {
  oar << var.load();
}

template <typename T, typename Archive>
void __serialize_var_(const T &var, Archive &oar) {
  oar << var;
}

}  // namespace Common
}  // namespace Huawei

///////////////////////////////////////////////////////////////////
// Memory Dump Macros:                                           //
///////////////////////////////////////////////////////////////////
//
// Encode Macros:

// ODD_ENCODE: The macro takes as input a list of Class members and
// the name of the class. The list of class members is pre-processed
// according to whether the member is a variable or a function, i.e.,
// using ODD_EN_SIMPLE or ODD_EN_CALL_SIMPLE.
// Adds the definition of two functions to the class definition body:
//   onDemandDebugCB: generic function used as callback function
//   called by perfpublisher upon request. The function takes a
//   reference of the same type of the Class (was kept in the perf
//   registery). Also, takes a boost::archive (DumpMemoArchive) which
//    is passed to the __odd_encode function.
//   __odd_encode: Takes as input a boost::archive (oar) which
//   handles serialization in a transparent way for several data types
//   e.g. oar << fname() or __serialize_var_(varname, oar)
#define ODD_ENCODE(BODY, CLASS)                                        \
 public:                                                               \
  Huawei::Common::CounterNodeRegistration _dnReg;                      \
  static char *onDemandDebugCB(void *handle,                           \
                               Huawei::Common::DumpMemoArchive &oar) { \
    CLASS *t = reinterpret_cast<CLASS *>(handle);                      \
    return t->__odd_encode(oar);                                       \
  }                                                                    \
  char *__odd_encode(Huawei::Common::DumpMemoArchive &oar) {           \
    BODY;                                                              \
    return nullptr;                                                    \
  }

#define ODD_EN_SIMPLE(varname) __serialize_var_(varname, oar);
#define ODD_EN_CALL_SIMPLE(fname) oar << fname();

// This macro routes to ODD_EN_SIMPLE ODD_EN_CALL_SIMPLE
// depending on user hint SIMPLE vs CALL_SIMPLE which identifies
// whether the subject is a variable or function
#define ODD_ENCODER(VARNAME, TYPE) ODD_EN_##TYPE(VARNAME)

///////////////////////////////////////////////////////////////////
// Print and DECODE Macros:

// ODD_PRINT: The Macro takes as input DEFS, DECODE, PRINT, CLASS
//   DEFS: During deserialization, we want to define a local variable,
//   with the same data type of a member function. So we can decode to
//   it then print it.
//   DECODE: A list of member names pre-processed according to
//   ODD_DECODER. Responsible for deserialization of members that
//   are not functions.
//   PRINT: A list of member names pre-processed according to
//   ODD_PRINTER.
//   CLASS: the name of the Class (as type).
//   The function adds two functions to the body of the Class:
//  __toString: a generic function takes as input a boost:archive
//  (DumpMemiArchive) which is called offline during decoding.
//  _toString: a custom function which is responsible for
//  deserialization and printing
//
// Note: use ::operator new and ::operator delete to throw exception while alloc
// memory failed
#define ODD_PRINT(DEFS, DECODE, PRINT, CLASS)                                  \
 public:                                                                       \
  static std::string __toString(Huawei::Common::DumpMemiArchive &iar) {        \
    CLASS *myclass = reinterpret_cast<CLASS *>(::operator new(sizeof(CLASS))); \
    std::string ret = myclass->_toString(iar);                                 \
    ::operator delete(myclass);                                                \
    return ret;                                                                \
  }                                                                            \
  std::string _toString(Huawei::Common::DumpMemiArchive &iar) {                \
    DECODE                                                                     \
    std::stringstream ss;                                                      \
    PRINT;                                                                     \
    DEFS;                                                                      \
    return ss.str();                                                           \
  }

#define ODD_DE_SIMPLE(varname) __deserialize_var_(varname, iar);
#define ODD_DE_CALL_SIMPLE(fname)

// This macro routes to ODD_EN_SIMPLE ODD_EN_CALL_SIMPLE
// depending on user hint SIMPLE vs CALL_SIMPLE which identifies
// whether the subject is a variable or function.
// Note: ODD_DE_CALL_SIMPLE does nothing because member is handled by
// ODD_DF_CALL_SIMPLE.
#define ODD_DECODER(VARNAME, TYPE) ODD_DE_##TYPE(VARNAME)

#define ODD_P_SIMPLE(varname) \
  ss << #varname ": " << Huawei::Common::stl_to_string(varname) << std::endl;
#define ODD_P_CALL_SIMPLE(fname)

// This macro routes to ODD_PRINTER to ODD_P_SIMPLE and
// ODD_P_CALL_SIMPLE depending on user hint SIMPLE vs CALL_SIMPLE.
// Note: ODD_P_CALL_SIMPLE because member is handled by ODD_DF_CALL_SIMPLE.
#define ODD_PRINTER(VARNAME, TYPE) ODD_P_##TYPE(VARNAME)

#define ODD_DF_SIMPLE(varname)
#define ODD_DF_CALL_SIMPLE(fname)                                     \
  {                                                                   \
    decltype((((decltype(this))0)->*&fname)()) ODD_FNAME2VARNAME_;    \
    iar >> ODD_FNAME2VARNAME_;                                        \
    ss << #fname                                                      \
       << "(): " << Huawei::Common::stl_to_string(ODD_FNAME2VARNAME_) \
       << std::endl;                                                  \
  }

// This macro routes to ODD_DEFINER to ODD_DF_SIMPLE and
// ODD_DF_CALL_SIMPLE depending on user hint SIMPLE vs CALL_SIMPLE.
// For functions, we need to declare a local variable in the body
// of the decoding function, so it can be used during serialization
// and printing. Nothing to do for SIMPLE types because it was handled
// by ODD_DE_SIMPLE and ODD_P_SIMPLE.
// TODO: room for optimization, can get rid ODD_DF_SIMPLE and so
// DEFINE, by using ODD_P_CALL_SIMPLE and ODD_DE_CALL_SIMPLE.
#define ODD_DEFINER(VARNAME, TYPE) ODD_DF_##TYPE(VARNAME)

///////////////////////////////////////////////////////////////////
// ODD_INIT Macro:
//
// This Macro is meant to be called in .h inside the class definition
// The Macro will drive ODD_ENCODE, ODD_PRINT which will define
// the decode/encode functions.

#define ODD_INIT(CLASS, DUMP_POINT)                           \
  ODD_ENCODE(DUMP_POINT(ODD_ENCODER), CLASS);                 \
  ODD_PRINT(DUMP_POINT(ODD_DEFINER), DUMP_POINT(ODD_DECODER), \
            DUMP_POINT(ODD_PRINTER), CLASS);
// This macro is ment to be called once in the constructor to register
// the Class instance in the perf register.
#define ODD_INIT_CC(CLASS, INSTANCE_ID_STR) \
  std::string cname(#CLASS);                \
  _dnReg.registerDumpMem(this, INSTANCE_ID_STR, CLASS::onDemandDebugCB, cname);
#else

#define ODD_INIT(CLASS, DUMP_POINT)
#define ODD_INIT_CC(CLASS, INSTANCE_ID_STR)

#endif  // define ENABLE_ODD_MEMDUMP

#endif  //__DUMPMEM_H__
