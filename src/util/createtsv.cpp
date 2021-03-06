#include "Parameters.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "Util.h"

#ifdef OPENMP
#include <omp.h>
#endif

#ifndef SIZE_T_MAX
#define SIZE_T_MAX ((size_t) -1)
#endif
int createtsv(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, 3, true, Parameters::PARSE_VARIADIC);

    Debug(Debug::INFO) << "Query database: " << par.db1 << "\n";
    DBReader<unsigned int> queryDB(par.hdr1.c_str(), par.hdr1Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    queryDB.open(DBReader<unsigned int>::NOSORT);
    queryDB.readMmapedDataInMemory();
    unsigned int* qHeaderLength = queryDB.getSeqLens();

    const bool hasTargetDB = par.filenames.size() > 3;
    bool sameDatabase = false;
    DBReader<unsigned int> *targetDB = NULL;
    unsigned int* tHeaderLength = NULL;
    if (hasTargetDB) {
        if (par.db1 == par.db2) {
            sameDatabase = true;
            targetDB = &queryDB;
        } else {
            Debug(Debug::INFO) << "Target database: " << par.db2 << "\n";
            targetDB = new DBReader<unsigned int>(par.hdr2.c_str(), par.hdr2Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
            targetDB->open(DBReader<unsigned int>::NOSORT);
            targetDB->readMmapedDataInMemory();
            tHeaderLength = targetDB->getSeqLens();
        }
    }

    DBReader<unsigned int> *reader;
    if (hasTargetDB) {
        Debug(Debug::INFO) << "Result database: " << par.db3 << "\n";
        reader = new DBReader<unsigned int>(par.db3.c_str(), par.db3Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    } else {
        Debug(Debug::INFO) << "Result database: " << par.db2 << "\n";
        reader = new DBReader<unsigned int>(par.db2.c_str(), par.db2Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    }
    reader->open(DBReader<unsigned int>::LINEAR_ACCCESS);

    const std::string& dataFile = hasTargetDB ? par.db4 : par.db3;
    const std::string& indexFile = hasTargetDB ? par.db4Index : par.db3Index;
    const bool shouldCompress = par.dbOut == true && par.compressed == true;
    const int dbType = par.dbOut == true ? Parameters::DBTYPE_GENERIC_DB : Parameters::DBTYPE_OMIT_FILE;
    Debug(Debug::INFO) << "Start writing to " << dataFile << "\n";
    DBWriter writer(dataFile.c_str(), indexFile.c_str(), par.threads, shouldCompress, dbType);
    writer.open();

    const size_t targetColumn = (par.targetTsvColumn == 0) ? SIZE_T_MAX :  par.targetTsvColumn - 1;
#pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif

        char **columnPointer = new char*[255];
        char *dbKey = new char[par.maxSeqLen + 1];

        std::string outputBuffer;
        outputBuffer.reserve(10 * 1024);

#pragma omp for schedule(dynamic, 1000)
        for (size_t i = 0; i < reader->getSize(); ++i) {
            unsigned int queryKey = reader->getDbKey(i);
            size_t queryIndex = queryDB.getId(queryKey);

            char *headerData = queryDB.getData(queryIndex, thread_idx);
            if (headerData == NULL) {
                Debug(Debug::WARNING) << "Invalid header entry in query " << queryKey << "!\n";
                continue;
            }

            std::string queryHeader;
            if (par.fullHeader) {
                queryHeader = "\"";
                queryHeader.append(headerData, qHeaderLength[queryIndex] - 2);
                queryHeader.append("\"");
            } else {
                queryHeader = Util::parseFastaHeader(headerData);
            }

            size_t entryIndex = 0;

            char *data = reader->getData(i, thread_idx);
            while (*data != '\0') {
                if(targetColumn != SIZE_T_MAX){
                    size_t foundElements = Util::getWordsOfLine(data, columnPointer, 255);
                    if (foundElements < targetColumn) {
                        Debug(Debug::WARNING) << "Not enough columns!" << "\n";
                        continue;
                    }
                    Util::parseKey(columnPointer[targetColumn], dbKey);
                }
                std::string targetAccession;
                if(targetColumn == SIZE_T_MAX){
                    targetAccession="";
                } else if (targetDB != NULL) {
                    unsigned int targetKey = (unsigned int) strtoul(dbKey, NULL, 10);
                    size_t targetIndex = targetDB->getId(targetKey);
                    char *targetData = targetDB->getData(targetIndex, thread_idx);
                    if (targetData == NULL) {
                        Debug(Debug::WARNING) << "Invalid header entry in query " << queryKey << " and target " << targetKey << "!\n";
                        continue;
                    }
                    if (par.fullHeader) {
                        targetAccession = "\"";
                        targetAccession.append(targetData, tHeaderLength[targetIndex] - 2);
                        targetAccession.append("\"");
                    } else {
                        targetAccession = Util::parseFastaHeader(targetData);
                    }
                } else {
                    targetAccession = dbKey;
                }

                if (par.firstSeqRepr && !entryIndex) {
                    queryHeader = targetAccession;
                }

                outputBuffer.append(queryHeader);
                outputBuffer.append("\t");
                outputBuffer.append(targetAccession);

                size_t offset = 0;
                if (targetColumn != 0) {
                    outputBuffer.append("\t");
                    offset = 0;
                } else {
                    offset = strlen(dbKey);
                }

                char *nextLine = Util::skipLine(data);
                outputBuffer.append(data + offset, (nextLine - (data + offset)) - 1);
                outputBuffer.append("\n");
                data = nextLine;
                entryIndex++;
            }
            writer.writeData(outputBuffer.c_str(), outputBuffer.length(), queryKey, thread_idx, par.dbOut);
            outputBuffer.clear();
        }
        delete[] dbKey;
        delete[] columnPointer;
    }
    writer.close();
    Debug(Debug::INFO) << "Done.\n";

    if (par.dbOut == false) {
        if (hasTargetDB) {
            std::remove(par.db4Index.c_str());
        } else {
            std::remove(par.db3Index.c_str());
        }
    }

    reader->close();
    delete reader;
    if (sameDatabase == false && targetDB != NULL) {
        targetDB->close();
        delete targetDB;
    }
    queryDB.close();

    return EXIT_SUCCESS;
}
#undef SIZE_T_MAX
