#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "logical_order_persistence.hpp"
#include "compact_document_part00.inc"
#include "compact_document_part01.inc"
#include "compact_document_part02.inc"
#include "logical_order_persistence_part00.inc"
#include "logical_order_publication_part00.inc"
#include "compact_document_part03.inc"
#include "order_statistics_sequence_part00.inc"
#include "order_statistics_sequence_part01.inc"
#include "order_statistics_sequence_part02.inc"
#include "order_statistics_sequence_part03.inc"
#include "order_statistics_sequence_fork.inc"
