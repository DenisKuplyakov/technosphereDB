#include "CachedPageReadWriter.h"

#include <string>
#include <cstring>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>

const char CachedPageReadWriter::LOG_ACTION_CHANGE[CachedPageReadWriter::LOG_ACTION_SIZE] = "CHANGE_";
const char CachedPageReadWriter::LOG_ACTION_DB_OPEN[CachedPageReadWriter::LOG_ACTION_SIZE] = "DB_OPEN";
const char CachedPageReadWriter::LOG_ACTION_DB_CLOSE[CachedPageReadWriter::LOG_ACTION_SIZE] = "DBCLOSE";
const char CachedPageReadWriter::LOG_ACTION_CHECKPOINT[CachedPageReadWriter::LOG_ACTION_SIZE] = "CHCKPNT";
const char CachedPageReadWriter::LOG_SEEK_DELIM[CachedPageReadWriter::LOG_SEEK_DELIM_SIZE] = {'|'};

CachedPageReadWriter::CachedPageReadWriter(PageReadWriter *source, GlobalConfiguration *globConf)
    : m_globConf(globConf)
    , m_source(source)
    , m_writesCounter(0)
{
    if (m_globConf->cacheSize() % m_globConf->pageSize()) {
	throw std::string("Page size should divide cache size.");
    }

    size_t cacheCells = m_globConf->cacheSize() / m_globConf->pageSize();
    m_cache.assign(cacheCells, nullptr);
    m_isDirty.assign(cacheCells, false);
    for (size_t i = 0; i < cacheCells; i++) {
	m_lruList.push_back(i);
    }
    // m_posInCache already empty

    bool noJournalBefore = access(globConf->journalPath(), F_OK) == -1;
    m_logFd = open(globConf->journalPath(), O_RDWR | O_CREAT, 0666);
    if (noJournalBefore) {
	::write(m_logFd, LOG_ACTION_CHECKPOINT, LOG_ACTION_SIZE);
	writeLogStumb();
    } else {
	size_t recordSize = LOG_ACTION_SIZE + sizeof(size_t) + m_globConf->pageSize();
	lseek(m_logFd, 0, SEEK_END);
	char recordType[LOG_ACTION_SIZE];
	do {
	    lseek(m_logFd, -static_cast<off_t>(recordSize), SEEK_CUR);
	    ::read(m_logFd, recordType, LOG_ACTION_SIZE);
	    lseek(m_logFd, -static_cast<off_t>(LOG_ACTION_SIZE), SEEK_CUR);
	} while (strcmp(recordType, LOG_ACTION_CHECKPOINT));
	// Now pointing begin of check point, lets skip it
	lseek(m_logFd, recordSize, SEEK_CUR);
	while (::read(m_logFd, recordType, LOG_ACTION_SIZE) == LOG_ACTION_SIZE) {
	    if (!strcmp(recordType, LOG_ACTION_CHANGE)) {
		size_t pageNumber;
		::read(m_logFd, &pageNumber, sizeof(pageNumber));
		Page p(pageNumber, m_globConf->pageSize());
		::read(m_logFd, p.rawData(), m_globConf->pageSize());

		m_source->write(p);
	    } else {
		lseek(m_logFd, recordSize - LOG_ACTION_SIZE, SEEK_CUR); // skip this entry
	    }
	}
    }

    lseek(m_logFd, 0, SEEK_END);
    ::write(m_logFd, LOG_ACTION_DB_OPEN, LOG_ACTION_SIZE);
    writeLogStumb();
}

size_t CachedPageReadWriter::allocatePageNumber()
{
    return m_source->allocatePageNumber();
}

void CachedPageReadWriter::deallocatePageNumber(const size_t &number)
{
    std::map<size_t, size_t>::iterator it = m_posInCache.find(number);
    if (it != m_posInCache.end()) {
	delete m_cache[it->second];
	m_cache[it->second] = nullptr;
	m_isDirty[it->second] = false;

	m_lruList.erase(std::find(m_lruList.begin(), m_lruList.end(), it->second));
	m_lruList.push_back(it->second); // pushing to oldest to use first

	m_posInCache.erase(it);
    }
    m_source->deallocatePageNumber(number);
}

void CachedPageReadWriter::read(Page &page)
{
    std::map<size_t, size_t>::iterator it = m_posInCache.find(page.number());
    if (it == m_posInCache.end()) { // no page in cache
	size_t freeCachePos = freeCachePosition(); // will do poping if needed
	m_cache[freeCachePos] = new Page(page.number(), m_globConf->pageSize());
	m_posInCache[page.number()] = freeCachePos;
	m_isDirty[freeCachePos] = false;

	m_source->read(*m_cache[freeCachePos]);
    }
    size_t cachePos = m_posInCache[page.number()];
    memcpy(page.rawData(), m_cache[cachePos]->rawData(), m_globConf->pageSize());

    m_lruList.erase(std::find(m_lruList.begin(), m_lruList.end(), cachePos));
    m_lruList.push_front(cachePos);
}

void CachedPageReadWriter::write(const Page &page)
{
    if (m_writesCounter == CHECKPOINT_THRESHOLD) {
	m_writesCounter = 0;
	flush();
    } else {
	m_writesCounter++;
    }

    ::write(m_logFd, LOG_ACTION_CHANGE, LOG_ACTION_SIZE);
    size_t pageNumber = page.number();
    ::write(m_logFd, &pageNumber, sizeof(pageNumber));
    ::write(m_logFd, page.rawData(), m_globConf->pageSize());

    std::map<size_t, size_t>::iterator it = m_posInCache.find(page.number());
    if (it == m_posInCache.end()) { // no page in cache
	size_t freeCachePos = freeCachePosition(); // will do poping if needed
	m_cache[freeCachePos] = new Page(page.number(), m_globConf->pageSize());
	m_posInCache[page.number()] = freeCachePos;
    }
    size_t cachePos = m_posInCache[page.number()];
    m_isDirty[cachePos] = true;
    memcpy(m_cache[cachePos]->rawData(), page.rawData(), m_globConf->pageSize());
    flushCacheCell(cachePos);

    m_lruList.erase(std::find(m_lruList.begin(), m_lruList.end(), cachePos));
    m_lruList.push_front(cachePos);
}

void CachedPageReadWriter::close()
{
    flush();
    m_source->close();

    ::write(m_logFd, LOG_ACTION_DB_CLOSE, LOG_ACTION_SIZE);
    writeLogStumb();

    ::close(m_logFd);
}

void CachedPageReadWriter::flushCacheCell(size_t cachePos)
{
    if (m_isDirty[cachePos]) {
	m_source->write(*m_cache[cachePos]);
	m_isDirty[cachePos] = false;
    }
}

void CachedPageReadWriter::flush()
{
    for (const std::pair<size_t, size_t> &p : m_posInCache) {
	flushCacheCell(p.second);
    }
    m_source->flush();

    ::write(m_logFd, LOG_ACTION_CHECKPOINT, LOG_ACTION_SIZE);
    writeLogStumb();
}

size_t CachedPageReadWriter::freeCachePosition()
{
    size_t cachePos = m_lruList.back();

    if (m_cache[cachePos] != nullptr) {
	flushCacheCell(cachePos);
	m_posInCache.erase(m_cache[cachePos]->number());
	delete m_cache[cachePos];
	m_cache[cachePos] = nullptr;
    }

    return cachePos;
}

void CachedPageReadWriter::writeLogStumb()
{
    lseek(m_logFd, sizeof(size_t) + m_globConf->pageSize() - LOG_SEEK_DELIM_SIZE, SEEK_CUR); // Seek forward other fields
    ::write(m_logFd, LOG_SEEK_DELIM, LOG_SEEK_DELIM_SIZE);
}
