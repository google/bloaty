/* Copyright 2017 - 2021 R. Thomas
 * Copyright 2017 - 2021 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


enum class PE_SECTION_TYPES : uint8_t {
  TEXT       = 0,
  TLS        = 1,
  IMPORT     = 2,
  DATA       = 3,
  BSS        = 4,
  RESOURCE   = 5,
  RELOCATION = 6,
  EXPORT     = 7,
  DEBUG      = 8,
  LOAD_CONFIG = 9,
  UNKNOWN     = 10
};

enum class PE_TYPE : uint16_t {
    PE32      = 0x10b, ///< 32bits
    PE32_PLUS = 0x20b  ///< 64 bits
};

