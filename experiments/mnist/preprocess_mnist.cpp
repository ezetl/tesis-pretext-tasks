#include <iostream>
#include <fstream>
#include <string> 
#include <vector>
#include <cinttypes>
#include <sys/stat.h>
#include <iomanip>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <lmdb.h>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"

#include "caffe/proto/caffe.pb.h"

/*
 * Author: Ezequiel Torti Lopez
 *
 * This code parses the MNIST dataset files (images and labels).
 * It was done for my low-endian machine, but you can set the LOW_ENDIAN
 * flag off and it will run in high endian mode
 *
 * How to compile:
 *     g++ -o preprocess_mnist preprocess_mnist.cpp -std=gnu++11 -lopencv_core -lopencv_highgui 
 *
 */

using namespace caffe;
using namespace std;
using namespace cv;

#define LOW_ENDIAN true
#define TB 1099511627776

#define DATA_ROOT    "../data/"
#define TRAIN_IMAGES (DATA_ROOT"train-images-idx3-ubyte")
#define TRAIN_LABELS (DATA_ROOT"train-labels-idx1-ubyte")
#define TEST_IMAGES  (DATA_ROOT"t10k-images-idx3-ubyte")
#define TEST_LABELS  (DATA_ROOT"t10k-labels-idx1-ubyte")

#define LMDB_TRAIN (DATA_ROOT"mnist_train_lmdb/")
#define LMDB_VAL   (DATA_ROOT"mnist_val_lmdb/")

typedef char Byte;
typedef unsigned char uByte;
typedef struct
{
    uint32_t magic;
    uint32_t num_elems;
    uint32_t cols;
    uint32_t rows;
} MNIST_metadata;

void create_lmdbs(const char* images, const char* labels, const char* db_path);
uint32_t get_uint32_t(ifstream &f, streampos offset);
vector<uByte> read_block(ifstream &f, unsigned int size, streampos offset);
MNIST_metadata parse_images_header(ifstream &f);
MNIST_metadata parse_labels_header(ifstream &f);
void parse_images_data(ifstream &f, MNIST_metadata meta, vector<Mat> &mnist);
void parse_labels_data(ifstream &f, MNIST_metadata meta, vector<uByte> &labels);
vector<Mat> load_images(string path);
vector<uByte> load_labels(string path);
void process_images();
void process_labels();

int main(int argc, char** argv)
{
    cout << "Creating train LMDB\n";
    create_lmdbs(TRAIN_IMAGES, TRAIN_LABELS, LMDB_TRAIN);
    cout << "Creating test LMDB\n";
    create_lmdbs(TEST_IMAGES, TEST_LABELS, LMDB_VAL);
    return 0;
}

void create_lmdbs(const char* images, const char* labels, const char* lmdb_path)
{
    /*LMDB related code was taken from Caffe script convert_mnist_data.cpp*/
    // lmdb
    MDB_env *mdb_env;
    MDB_dbi mdb_dbi;
    MDB_val mdb_key, mdb_data;
    MDB_txn *mdb_txn;

    // Set database environment
    mkdir(lmdb_path, 0744);

    mdb_env_create(&mdb_env);
    mdb_env_set_mapsize(mdb_env, TB);
    mdb_env_open(mdb_env, lmdb_path, 0, 0664);
    mdb_txn_begin(mdb_env, NULL, 0, &mdb_txn);
    mdb_open(mdb_txn, NULL, 0, &mdb_dbi);

    // Load images/labels
    vector<Mat> list_imgs = load_images(images);
    vector<uByte> list_labels = load_labels(labels);
    // TODO: add random rotation/translations.
    // TODO: modify dimensions of Datum according to the new format of images (I'll do a Split to separate images later on training)

    // Storing to db
    unsigned int rows = list_imgs[0].rows;
    unsigned int cols = list_imgs[0].cols;
    int count = 0;
    string value;
    
    Datum datum;
    datum.set_channels(1);
    datum.set_height(rows);
    datum.set_width(cols);

    std::ostringstream s;
    for (unsigned int item_id = 0; item_id < list_imgs.size(); ++item_id) {
        datum.set_data((char*)list_imgs[item_id].data, rows*cols);
        datum.set_label((char)list_labels[item_id]);

        s << std::setw(8) << std::setfill('0') << item_id;
        string key_str = s.str();
        s.str(std::string());

        datum.SerializeToString(&value);

        mdb_data.mv_size = value.size();
        mdb_data.mv_data = reinterpret_cast<void*>(&value[0]);
        mdb_key.mv_size = key_str.size();
        mdb_key.mv_data = reinterpret_cast<void*>(&key_str[0]);
        mdb_put(mdb_txn, mdb_dbi, &mdb_key, &mdb_data, 0);
        if (++count % 1000 == 0) {
            // Commit txn
            mdb_txn_commit(mdb_txn);
            mdb_txn_begin(mdb_env, NULL, 0, &mdb_txn);
        }
    }
    // Last batch
    if (count % 1000 != 0) {
        mdb_txn_commit(mdb_txn);
        mdb_close(mdb_env, mdb_dbi);
        mdb_env_close(mdb_env);
    }

    return;
}

vector<Mat> load_images(string path)
{
    ifstream f;
    f.open(path, ios::in | ios::binary);

    MNIST_metadata meta = parse_images_header(f);
    cout << "\nMagic number: " << meta.magic << endl; 
    cout << "Number of Images: " << meta.num_elems << endl; 
    cout << "Rows: " << meta.rows << endl;
    cout << "Columns: " << meta.cols << endl; 
    vector<Mat> mnist(meta.num_elems);
    parse_images_data(f, meta, mnist);
    return mnist;
}

vector<uByte> load_labels(string path)
{
    ifstream f;
    f.open(path, ios::in | ios::binary);

    MNIST_metadata meta = parse_labels_header(f);
    cout << "\nMagic number: " << meta.magic << endl; 
    cout << "Number of Labels: " << meta.num_elems << endl; 
    vector<uByte> labels_mnist(meta.num_elems);
    parse_labels_data(f, meta, labels_mnist);
    return labels_mnist;

}

MNIST_metadata parse_images_header(ifstream &f)
{
    MNIST_metadata meta;
    streampos offset = 0;
    meta.magic = get_uint32_t(f, offset);
    offset += sizeof(uint32_t);
    meta.num_elems = get_uint32_t(f, offset);
    offset += sizeof(uint32_t);
    meta.rows = get_uint32_t(f, offset);
    offset += sizeof(uint32_t);
    meta.cols = get_uint32_t(f, offset);
    return meta;
}

void parse_images_data(ifstream &f, MNIST_metadata meta, vector<Mat> &mnist)
{
    unsigned int size_img = meta.cols * meta.rows;
    // 4 integers in the header of the images file
    streampos offset = sizeof(uint32_t) * 4;
    for (unsigned int i=0; i<meta.num_elems; i++)
    {
        vector<uByte> raw_data = read_block(f, size_img, offset);
        Mat mchar(raw_data, false);
        mchar = mchar.reshape(1, meta.rows);
        mnist[i] = mchar;
        offset += size_img;
    }
}

MNIST_metadata parse_labels_header(ifstream &f)
{
    MNIST_metadata meta;
    streampos offset = 0;
    meta.magic = get_uint32_t(f, offset);
    offset += sizeof(uint32_t);
    meta.num_elems = get_uint32_t(f, offset);
    return meta;
}

void parse_labels_data(ifstream &f, MNIST_metadata meta, vector<uByte> &labels)
{
    // 4 integers in the header of the images file
    streampos offset = sizeof(uint32_t) * 2;
    for (unsigned int i=0; i<meta.num_elems; i++)
    {
        f.seekg(offset);
        uByte label;
        f.read((Byte*) &label, sizeof(uByte));
        labels[i] = label;
        offset += sizeof(uByte);
    }
}

vector<uByte> read_block(ifstream &f, unsigned int size, streampos offset)
{
    Byte* bytes; 
    bytes = (Byte*) malloc(size*sizeof(uByte));

    f.seekg(offset);
    f.read(bytes, size);

    vector<uByte> raw_data(size);
    for (unsigned int i=0; i<size; i++)
    {
        raw_data[i] = (uByte) bytes[i];
    }

    free(bytes);

    return raw_data;
}
 
/*
 * It parses a int (32 bits) from the file f.
 * The MNIST dataset uses big-endian. This function take into account
 * wheter the local architecture is {big,little}-endian and return 
 * the correct interpretation of the integer.
 * 
 * Precondition: f has to be opened with ios::in | ios::binary flags
 */
uint32_t get_uint32_t(ifstream &f, streampos offset)
{
    // TODO add support to big-endian machines
    uint32_t* i_int;
    Byte* b_int; 

    b_int = (Byte*) malloc(sizeof(uint32_t));

    for (unsigned int i=0; i<sizeof(uint32_t); i++)
    {
        f.seekg(offset + (streampos) i);
        f.read(b_int+(sizeof(uint32_t)-i-1), sizeof(Byte));
    }
    i_int = reinterpret_cast<uint32_t*>(b_int); 

    uint32_t res = *i_int;
    free(b_int);

    return res;
}

void process_images()
{

}

void process_labels()
{

}
