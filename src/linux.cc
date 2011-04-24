/*
 * libjingle
 * Copyright 2008 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "linux.h"

#ifdef LINUX

#include <errno.h>
#include <sys/utsname.h>

#include <cstdio>
#include <set>

#include "stringencode.h"

namespace txmpp {

static const char kCpuInfoFile[] = "/proc/cpuinfo";
static const char kCpuMaxFreqFile[] =
    "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq";

ProcCpuInfo::ProcCpuInfo() {
}

ProcCpuInfo::~ProcCpuInfo() {
}

bool ProcCpuInfo::LoadFromSystem() {
  ConfigParser procfs;
  if (!procfs.Open(kCpuInfoFile)) {
    return false;
  }
  return procfs.Parse(&sections_);
};

bool ProcCpuInfo::GetSectionCount(size_t* count) {
  if (sections_.empty()) {
    return false;
  }
  if (count) {
    *count = sections_.size();
  }
  return true;
}

bool ProcCpuInfo::GetNumCpus(int* num) {
  if (sections_.empty()) {
    return false;
  }
  int total_cpus = 0;
#if defined(__arm__)
  // Count the number of blocks that have a "processor" key defined. On ARM,
  // there may be extra blocks of information that aren't per-processor.
  size_t section_count = sections_.size();
  for (size_t i = 0; i < section_count; ++i) {
    int processor_id;
    if (GetSectionIntValue(i, "processor", &processor_id)) {
      ++total_cpus;
    }
  }
  // Single core ARM systems don't include "processor" keys at all, so return
  // that we have a single core if we didn't find any explicitly above.
  if (total_cpus == 0) {
    total_cpus = 1;
  }
#else
  // On X86, there is exactly one info section per processor.
  total_cpus = static_cast<int>(sections_.size());
#endif
  if (num) {
    *num = total_cpus;
  }
  return true;
}

bool ProcCpuInfo::GetNumPhysicalCpus(int* num) {
  if (sections_.empty()) {
    return false;
  }
  // TODO: /proc/cpuinfo only reports cores that are currently
  // _online_, so this may underreport the number of physical cores.
#if defined(__arm__)
  // ARM (currently) has no hyperthreading, so just return the same value
  // as GetNumCpus.
  return GetNumCpus(num);
#else
  int total_cores = 0;
  std::set<int> physical_ids;
  size_t section_count = sections_.size();
  for (size_t i = 0; i < section_count; ++i) {
    int physical_id;
    int cores;
    // Count the cores for the physical id only if we have not counted the id.
    if (GetSectionIntValue(i, "physical id", &physical_id) &&
        GetSectionIntValue(i, "cpu cores", &cores) &&
        physical_ids.find(physical_id) == physical_ids.end()) {
      physical_ids.insert(physical_id);
      total_cores += cores;
    }
  }

  if (num) {
    *num = total_cores;
  }
  return true;
#endif
}

bool ProcCpuInfo::GetSectionStringValue(size_t section_num,
                                        const std::string& key,
                                        std::string* result) {
  if (section_num >= sections_.size()) {
    return false;
  }
  ConfigParser::SimpleMap::iterator iter = sections_[section_num].find(key);
  if (iter == sections_[section_num].end()) {
    return false;
  }
  *result = iter->second;
  return true;
}

bool ProcCpuInfo::GetSectionIntValue(size_t section_num,
                                     const std::string& key,
                                     int* result) {
  if (section_num >= sections_.size()) {
    return false;
  }
  ConfigParser::SimpleMap::iterator iter = sections_[section_num].find(key);
  if (iter == sections_[section_num].end()) {
    return false;
  }
  return FromString(iter->second, result);
}

ConfigParser::ConfigParser() {}

ConfigParser::~ConfigParser() {}

bool ConfigParser::Open(const std::string& filename) {
  FileStream* fs = new FileStream();
  if (!fs->Open(filename, "r")) {
    return false;
  }
  instream_.reset(fs);
  return true;
}

void ConfigParser::Attach(StreamInterface* stream) {
  instream_.reset(stream);
}

bool ConfigParser::Parse(MapVector* key_val_pairs) {
  // Parses the file and places the found key-value pairs into key_val_pairs.
  SimpleMap section;
  while (ParseSection(&section)) {
    key_val_pairs->push_back(section);
    section.clear();
  }
  return (!key_val_pairs->empty());
}

bool ConfigParser::ParseSection(SimpleMap* key_val_pair) {
  // Parses the next section in the filestream and places the found key-value
  // pairs into key_val_pair.
  std::string key, value;
  while (ParseLine(&key, &value)) {
    (*key_val_pair)[key] = value;
  }
  return (!key_val_pair->empty());
}

bool ConfigParser::ParseLine(std::string* key, std::string* value) {
  // Parses the next line in the filestream and places the found key-value
  // pair into key and val.
  std::string line;
  if ((instream_->ReadLine(&line)) == EOF) {
    return false;
  }
  std::vector<std::string> tokens;
  if (2 != split(line, ':', &tokens)) {
    return false;
  }
  // Removes whitespace at the end of Key name
  size_t pos = tokens[0].length() - 1;
  while ((pos > 0) && isspace(tokens[0][pos])) {
    pos--;
  }
  tokens[0].erase(pos + 1);
  // Removes whitespace at the start of value
  pos = 0;
  while (pos < tokens[1].length() && isspace(tokens[1][pos])) {
    pos++;
  }
  tokens[1].erase(0, pos);
  *key = tokens[0];
  *value = tokens[1];
  return true;
}

static bool ExpectLineFromStream(FileStream* stream,
                                 std::string* out) {
  StreamResult res = stream->ReadLine(out);
  if (res != SR_SUCCESS) {
    if (res != SR_EOS) {
      LOG(LS_ERROR) << "Error when reading from stream";
    } else {
      LOG(LS_ERROR) << "Incorrect number of lines in stream";
    }
    return false;
  }
  return true;
}

static void ExpectEofFromStream(FileStream* stream) {
  std::string unused;
  StreamResult res = stream->ReadLine(&unused);
  if (res == SR_SUCCESS) {
    LOG(LS_WARNING) << "Ignoring unexpected extra lines from stream";
  } else if (res != SR_EOS) {
    LOG(LS_WARNING) << "Error when checking for extra lines from stream";
  }
}

// For caching the lsb_release output (reading it invokes a sub-process and
// hence is somewhat expensive).
static std::string lsb_release_string;
static CriticalSection lsb_release_string_critsec;

std::string ReadLinuxLsbRelease() {
  CritScope cs(&lsb_release_string_critsec);
  if (!lsb_release_string.empty()) {
    // Have cached result from previous call.
    return lsb_release_string;
  }
  // No cached result. Run lsb_release and parse output.
  POpenStream lsb_release_output;
  if (!lsb_release_output.Open("lsb_release -idrcs", "r")) {
    LOG_ERR(LS_ERROR) << "Can't run lsb_release";
    return lsb_release_string;  // empty
  }
  // Read in the command's output and build the string.
  std::ostringstream sstr;
  std::string line;
  int wait_status;

  if (!ExpectLineFromStream(&lsb_release_output, &line)) {
    return lsb_release_string;  // empty
  }
  sstr << "DISTRIB_ID=" << line;

  if (!ExpectLineFromStream(&lsb_release_output, &line)) {
    return lsb_release_string;  // empty
  }
  sstr << " DISTRIB_DESCRIPTION=\"" << line << '"';

  if (!ExpectLineFromStream(&lsb_release_output, &line)) {
    return lsb_release_string;  // empty
  }
  sstr << " DISTRIB_RELEASE=" << line;

  if (!ExpectLineFromStream(&lsb_release_output, &line)) {
    return lsb_release_string;  // empty
  }
  sstr << " DISTRIB_CODENAME=" << line;

  // Should not be anything left.
  ExpectEofFromStream(&lsb_release_output);

  lsb_release_output.Close();
  wait_status = lsb_release_output.GetWaitStatus();
  if (wait_status == -1 ||
      !WIFEXITED(wait_status) ||
      WEXITSTATUS(wait_status) != 0) {
    LOG(LS_WARNING) << "Unexpected exit status from lsb_release";
  }

  lsb_release_string = sstr.str();

  return lsb_release_string;
}

std::string ReadLinuxUname() {
  struct utsname buf;
  if (uname(&buf) < 0) {
    LOG_ERR(LS_ERROR) << "Can't call uname()";
    return std::string();
  }
  std::ostringstream sstr;
  sstr << buf.sysname << " "
       << buf.release << " "
       << buf.version << " "
       << buf.machine;
  return sstr.str();
}

int ReadCpuMaxFreq() {
  FileStream fs;
  std::string str;
  int freq = -1;
  if (!fs.Open(kCpuMaxFreqFile, "r") ||
      SR_SUCCESS != fs.ReadLine(&str) ||
      !FromString(str, &freq)) {
    return -1;
  }
  return freq;
}

}  // namespace txmpp

#endif  // LINUX
