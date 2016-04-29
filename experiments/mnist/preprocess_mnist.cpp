/*
 * This code parses the MNIST dataset files (images and labels).
 *
 * It was done for my low-endian machine, but you can set the LOW_ENDIAN
 * flag off and it will run in high endian mode (TODO)
 *
 * The main idea is to create a database with 5 million images to 
 * reproduce the results obtained in "Learning to See by Moving" by 
 * Agrawal el al.
 *
 * For that, I parse the MNIST data, apply the transformations mentioned
 * in the section 3.4.1 of the paper and save the images to a LMDB.
 *
 * Since Agrawal et al. pose the problem as a classification problem
 * (discrete clases for transformations in the axis X, Y and Z) we 
 * need three classifiers, one for each dimension; hence we need three 
 * labels. It is not straightforward to create a multiclass database in LMDB,
 * so I will create a separate database with an array of labels for each element.
 * Then, during training phase, I just Slice the label to retrieve the 
 * labels again. Check this post to get a more complete idea:
 * https://groups.google.com/forum/#!searchin/caffe-users/multilabel/caffe-users/RuT1TgwiRCo/hoUkZOeEDgAJ 
 *
 * This code is part of my undergrad thesis: "Reconocimiento visual
 * empleando técnicas de deep learning" ("Visual Recognition using Deep
 * Learning techniques")
 *
 * Author: Ezequiel Torti Lopez
 */

#include <iostream>
#include <fstream>
#include <string> 
#include <vector>
#include <cinttypes>
#include <sys/stat.h>
#include <stdlib.h> 
#include <iomanip>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <lmdb.h>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "caffe/proto/caffe.pb.h"


using namespace caffe;
using namespace std;
using namespace cv;

#define LOW_ENDIAN true
#define TB 1099511627776
#define NUM_TRASLATIONS 7
#define NUM_ROTATIONS 61
#define NUM_BIN_ROTATIONS 20
#define LOWER_ANGLE -30
#define LOWER_TRASLATION -3
#define BATCHES 20 
#define NUM_CLASSES 3

#define DATA_ROOT    "../data/"
#define TRAIN_IMAGES (DATA_ROOT"train-images-idx3-ubyte")
#define TRAIN_LABELS (DATA_ROOT"train-labels-idx1-ubyte")
#define TEST_IMAGES  (DATA_ROOT"t10k-images-idx3-ubyte")
#define TEST_LABELS  (DATA_ROOT"t10k-labels-idx1-ubyte")

#define LMDB_TRAIN        (DATA_ROOT"mnist_train_lmdb/")
#define LMDB_TRAIN_LABELS (DATA_ROOT"mnist_train_labels_lmdb/")
#define LMDB_VAL          (DATA_ROOT"mnist_val_lmdb/")

typedef char Byte;
typedef unsigned char uByte;
typedef uByte Label;
typedef struct
{
    uint32_t magic;
    uint32_t num_elems;
    uint32_t cols;
    uint32_t rows;
} MNIST_metadata;

typedef struct
{
    Mat img;
    Label x;
    Label y;
    Label z;
} DataBlob;

void create_lmdbs(const char* images, const char* labels, const char* lmdb_path, const char* lmdb_labels_path);
uint32_t get_uint32_t(ifstream &f, streampos offset);
vector<uByte> read_block(ifstream &f, unsigned int size, streampos offset);
MNIST_metadata parse_images_header(ifstream &f);
MNIST_metadata parse_labels_header(ifstream &f);
void parse_images_data(ifstream &f, MNIST_metadata meta, vector<Mat> *mnist);
void parse_labels_data(ifstream &f, MNIST_metadata meta, vector<Label> *labels);
Mat transform_image(Mat &img, float tx, float ty, float rot);
vector<Mat> load_images(string path);
vector<Label> load_labels(string path);
vector<DataBlob> process_images(vector<Mat> &list_imgs);
unsigned int generate_rand(int range_limit);

int main(int argc, char** argv)
{
    cout << "Creating train LMDB\n";
    create_lmdbs(TRAIN_IMAGES, TRAIN_LABELS, LMDB_TRAIN, LMDB_TRAIN_LABELS);
    return 0;
}

void create_lmdbs(const char* images, const char* labels, const char* lmdb_path, const char* lmdb_labels_path)
{
    /*LMDB related code was taken from Caffe script convert_mnist_data.cpp*/

    // lmdb data 
    MDB_env *mdb_env;
    MDB_dbi mdb_dbi;
    MDB_val mdb_key, mdb_data;
    MDB_txn *mdb_txn;

    // lmdb labels 
    MDB_env *mdb_label_env;
    MDB_dbi mdb_label_dbi;
    MDB_val mdb_label_key, mdb_labels;
    MDB_txn *mdb_label_txn;

    // Set database environment
    mkdir(lmdb_path, 0744);
    mkdir(lmdb_labels_path, 0744);

    mdb_env_create(&mdb_env);
    mdb_env_set_mapsize(mdb_env, TB);
    mdb_env_open(mdb_env, lmdb_path, 0, 0664);
    mdb_txn_begin(mdb_env, NULL, 0, &mdb_txn);
    mdb_open(mdb_txn, NULL, 0, &mdb_dbi);

    mdb_env_create(&mdb_label_env);
    mdb_env_set_mapsize(mdb_label_env, TB);
    mdb_env_open(mdb_label_env, lmdb_labels_path, 0, 0664);
    mdb_txn_begin(mdb_label_env, NULL, 0, &mdb_label_txn);
    mdb_open(mdb_label_txn, NULL, 0, &mdb_label_dbi);

    // Load images/labels
    vector<Mat> list_imgs = load_images(images);
    vector<Label> list_labels = load_labels(labels);

    // Storing to db
    unsigned int rows = list_imgs[0].rows;
    unsigned int cols = list_imgs[0].cols;
    int count = 0;
    string data_value, label_value;
    
    // Data datum
    Datum datum;
    datum.set_channels(2);
    datum.set_height(rows);
    datum.set_width(cols);

    // Labels datum
    Datum ldatum;
    datum.set_channels(1);
    datum.set_height(1);
    datum.set_width(NUM_CLASSES);

    std::ostringstream s;

    // Processing and generating 5 million images at once will consume too much RAM (>7GB)
    // and it will (probably) throw a std::bad_alloc exception.
    // Lets split the processing in several batches instead. 
    // list_imgs.size() has to be multiple of BATCHES (to simplify things)
    int len_batch = list_imgs.size() / BATCHES;
    for (unsigned int i = 0; i<BATCHES; i++)
    {
        unsigned int begin = i * len_batch; 
        unsigned int end = begin + len_batch;
        vector<Mat> batch_imgs = vector<Mat>(list_imgs.begin()+begin, list_imgs.begin()+end);
        vector<DataBlob> batch_data = process_images(batch_imgs);
        for (unsigned int item_id = 0; item_id < batch_data.size(); ++item_id) {
            // Set Data
            datum.set_data((char*)batch_data[item_id].img.data, rows*cols);
            // Set Labels
            char labels[3] = {(char)batch_data[item_id].x,
                            (char)batch_data[item_id].y,
                            (char)batch_data[item_id].z};
            ldatum.set_data(labels, NUM_CLASSES);

            // Dont use item_id as key here since we have 83/85 images per original image,
            // meaning that we will overwrite the same image 83/85 times instead of creating 
            // a new entry
            s << std::setw(8) << std::setfill('0') << count; 
            string key_str = s.str();
            s.str(std::string());

            datum.SerializeToString(&data_value);
            ldatum.SerializeToString(&label_value);

            // Sabe Data
            mdb_data.mv_size = data_value.size();
            mdb_data.mv_data = reinterpret_cast<void*>(&data_value[0]);
            mdb_key.mv_size = key_str.size();
            mdb_key.mv_data = reinterpret_cast<void*>(&key_str[0]);
            mdb_put(mdb_txn, mdb_dbi, &mdb_key, &mdb_data, 0);
            // Sabe Label 
            mdb_labels.mv_size = label_value.size();
            mdb_labels.mv_data = reinterpret_cast<void*>(&label_value[0]);
            mdb_label_key.mv_size = key_str.size();
            mdb_label_key.mv_data = reinterpret_cast<void*>(&key_str[0]);
            mdb_put(mdb_label_txn, mdb_label_dbi, &mdb_label_key, &mdb_labels, 0);
            if (++count % 1000 == 0) {
                // Commit txn Data
                mdb_txn_commit(mdb_txn);
                mdb_txn_begin(mdb_env, NULL, 0, &mdb_txn);
                // Commit txn Labels 
                mdb_txn_commit(mdb_label_txn);
                mdb_txn_begin(mdb_label_env, NULL, 0, &mdb_label_txn);
            }
        }
    }
    // Last batch
    if (count % 1000 != 0) {
        mdb_txn_commit(mdb_txn);
        mdb_txn_commit(mdb_label_txn);

        mdb_close(mdb_env, mdb_dbi);
        mdb_close(mdb_label_env, mdb_label_dbi);

        mdb_env_close(mdb_label_env);
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
    parse_images_data(f, meta, &mnist);
    return mnist;
}

vector<Label> load_labels(string path)
{
    ifstream f;
    f.open(path, ios::in | ios::binary);

    MNIST_metadata meta = parse_labels_header(f);
    cout << "\nMagic number: " << meta.magic << endl; 
    cout << "Number of Labels: " << meta.num_elems << endl; 
    vector<Label> labels_mnist(meta.num_elems);
    parse_labels_data(f, meta, &labels_mnist);
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

void parse_images_data(ifstream &f, MNIST_metadata meta, vector<Mat> *mnist)
{
    unsigned int size_img = meta.cols * meta.rows;
    // 4 integers in the header of the images file
    streampos offset = sizeof(uint32_t) * 4;
    for (unsigned int i=0; i<meta.num_elems; i++)
    {
        vector<uByte> raw_data = read_block(f, size_img, offset);
        Mat mchar(raw_data, false);
        mchar = mchar.reshape(1, meta.rows);
        (*mnist)[i] = mchar.clone();
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

void parse_labels_data(ifstream &f, MNIST_metadata meta, vector<Label> *labels)
{
    // 4 integers in the header of the images file
    streampos offset = sizeof(uint32_t) * 2;
    for (unsigned int i=0; i<meta.num_elems; i++)
    {
        f.seekg(offset);
        Label label;
        f.read((Byte*) &label, sizeof(Label));
        (*labels)[i] = label;
        offset += sizeof(Label);
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

/*
 * rot (Rotation) is in degrees
 * tx, ty (Translations) are pixels
 */
Mat transform_image(Mat &img, float tx, float ty, float rot)
{
    Mat res;
    Point2f mid(img.cols / 2, img.rows / 2);
    Mat rotMat = getRotationMatrix2D(mid,  rot,  1.0);
    Mat transMat = (Mat_<double>(2,3) << 0,0,tx,0,0,ty);
    rotMat = rotMat + transMat;
    // Set constant value for border to be white
    warpAffine(img,
               res,
               rotMat,
               Size(img.cols, img.rows),
               INTER_LINEAR,
               BORDER_CONSTANT,
               Scalar(0, 0, 0));
    return res;
}

vector<DataBlob> process_images(vector<Mat> &list_imgs)
{
    vector<DataBlob> final_data;
    srand(0);
    unsigned int rand_index = 0;
    vector<float> translations(NUM_TRASLATIONS);
    float value = LOWER_TRASLATION;
    for (unsigned int i=0; i<translations.size(); i++)
    {
        translations[i] = value++;
    }

    value = LOWER_ANGLE;
    vector<float> rotations(NUM_ROTATIONS);
    for (unsigned int i=0; i<rotations.size(); i++)
    {
        rotations[i] = value++;
    }

    // Debugging
    //namedWindow("Normal");
    //namedWindow("Transformed");
    for (unsigned int i=0; i<list_imgs.size(); i++)
    {
        unsigned int amount_pairs = 83;
        if (i<10000)
        {
            amount_pairs = 85;
        }

        // Use white background
        //bitwise_not(list_imgs[i], list_imgs[i]);

        for (unsigned int j=0; j<amount_pairs; j++)
        {
            DataBlob d;
            // Generate random X translation
            rand_index = generate_rand(NUM_TRASLATIONS);
            d.x = rand_index; 
            float tx = translations[rand_index];
            // Generate random Y translation
            rand_index = generate_rand(NUM_TRASLATIONS);
            d.y = rand_index; 
            float ty = translations[rand_index];
            // Calculate random bin of rotation (0 to 19)
            rand_index = generate_rand(NUM_BIN_ROTATIONS);
            d.z = rand_index;
            // Calculate the real index of the array of rotations (0 to 61)
            rand_index *= 3;
            rand_index += generate_rand(3);
            float rot = rotations[rand_index];

            // Finally, apply the selected transformations to the image
            Mat new_img = transform_image(list_imgs[i], tx, ty, rot);

            // Merge the original img and the transformed one into a unique Mat
            // Then we split the channels in Caffe using the SLICE layer
            auto channels = vector<Mat>{list_imgs[i], new_img};
            Mat merged_mats;
            merge(channels, merged_mats);
            d.img = merged_mats;

            final_data.push_back(d);

            // Debugging
            //imshow("Normal", list_imgs[i]);
            //imshow("Transformed", new_img);
            //waitKey(100);
        }
    }
    return final_data;
}

/* Generate a random number between 0 and range_limit-1
 * Useful to get a random element in an array of size range_limit
 */
unsigned int generate_rand(int range_limit)
{
    return rand() % range_limit;
}