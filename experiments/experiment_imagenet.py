#!/usr/bin/env python2.7
from collections import defaultdict
from os.path import join, exists
from os import makedirs
from optparse import OptionParser, OptionGroup
import pickle
from utils.nets.cnn_factory import KITTINetFactory
from utils.solver.solver import train_net, create_solver_params

"""
Script to reproduce the KITTI pretraining + Imagenet finetuning results.
"""


def parse_options():
    """
    Parse the command line options and returns the (options, arguments)
    tuple.

    The option -L is because I assume you have created all the LMDBs in
    the same folder. That makes things easier. Also, I assume you didn't
    change the original LMDB names (as they appear in the preprocessing
    scripts). If you did different, then feel free to change the
    lmdb_path and labels_lmdb_path parameters in the MNISTNetFactory
    and KITTINetFactory's methods calls
    """
    usage = "usage: %prog [options]"
    parser = OptionParser(usage=usage)
    parser.add_option("-L", "--lmdb-root", dest="lmdb_root", default='.',
            help="Root dir where all the LMDBs were created.", metavar="PATH")

    group_example = OptionGroup(parser, "Example:",
            './experiment_imagenet.py -L /media/eze/Datasets/KITTI/\\')
    parser.add_option_group(group_example)

    return parser.parse_args()


if __name__ == "__main__":
    (opts, args) = parse_options()

    scale = 1 
    batch_size = 50 
    iters = 60000
    results_path = './results/kitti/'
    try:
        makedirs(results_path)
    except:
        pass

    ## EGOMOTION NET
    ## Used to train a siamese network from scratch following the method from the
    ## paper
    siam_kitti, loss_blobs, acc_blobs = KITTINetFactory.siamese_egomotion(
            lmdb_path=join(opts.lmdb_root, 'KITTI/kitti_train_egomotion_lmdb'),
            labels_lmdb_path=join(opts.lmdb_root, 'KITTI/kitti_train_egomotion_lmdb_labels'),
            mean_file='/home/eze/Projects/tesis/experiments/datasets/data/mean_kitti_egomotion.binaryproto',
            batch_size=batch_size,
            scale=scale,
            is_train=True,
            learn_all=True
            )

    # Create a SolverParameter instance with the predefined parameters for this experiment.
    # Some paths and iterations numbers will change for nets with contrastive loss or
    # in finetuning stage
    iters = 60000
    # Train our first siamese net with Egomotion method
    if exists(join(results_path, 'egomotion.pickle')):
       with open(join(results_path, 'egomotion.pickle'), 'rb') as handle:
           results_ego = pickle.load(handle)
    else:
        results_ego = train_net(create_solver_params(siam_kitti, max_iter=iters, base_lr=0.001, stepsize=20000, snapshot_prefix='snapshots/imagenet/egomotion/kitti_siamese'),
                loss_blobs=loss_blobs)
        with open(join(results_path, 'egomotion.pickle'), 'wb') as handle:
            pickle.dump(results_ego, handle, protocol=pickle.HIGHEST_PROTOCOL)
    del siam_kitti

    # CONTRASTIVE NET, m=10
    # Using a small batch size while training with Contrastive Loss leads
    # to high bias in the networks (i.e. they dont learn much)
    # A good ad-hoc value is between 250-500
    siam_cont10_kitti, loss_cont_blobs, acc_cont_blobs = KITTINetFactory.siamese_contrastive(
            lmdb_path=join(opts.lmdb_root, 'KITTI/kitti_train_egomotion_lmdb'),
            batch_size=batch_size,
            scale=scale,
            contrastive_margin=10,
            is_train=True,
            learn_all=True
            )
    # Also, using a big lr (i.e. 0.01) while training with Contrastive Loss can lead to nan values while backpropagating the loss
    if exists(join(results_path, 'contr_10.pickle')):
        with open(join(results_path, 'contr_10.pickle'), 'rb') as handle:
            results_contr10 = pickle.load(handle)
    else:
        results_contr10 = train_net(create_solver_params(siam_cont10_kitti, max_iter=iters, base_lr=0.001, stepsize=20000, snapshot_prefix='snapshots/imagenet/contrastive/kitti_siamese_m10'),
                loss_blobs=loss_cont_blobs)
        with open(join(results_path, 'contr_10.pickle'), 'wb') as handle:
            pickle.dump(results_contr10, handle, protocol=pickle.HIGHEST_PROTOCOL)
    del siam_cont10_kitti

    acc = {key: defaultdict(int) for key in ['egomotion', 'cont_10', 'AlexNet']}
    sizes_lmdb = ['1', '5', '10', '20']
    iters = 10000
    for num in sizes_lmdb:
        # Finetune network
        imagenet_finetune, loss_blobs_f, acc_blobs_f = KITTINetFactory.standar(
                lmdb_path=join(opts.lmdb_root, 'ILSVRC12/ILSVRC12_Training_{}perclass_lmdb'.format(num)),
                batch_size=batch_size,
                scale=scale,
                num_classes=1000,
                is_train=True,
                learn_all=True,  # Lets train ALL the conv layers
                is_imagenet=True
                )

        # Test Net Used to test accuracy in finetunig stages
        imagenet_test, loss_blobs_test, acc_blobs_test = KITTINetFactory.standar(
                lmdb_path=join(opts.lmdb_root, 'ILSVRC12/ILSVRC12_Testing_{}perclass_lmdb'.format(num)),
                batch_size=batch_size,
                scale=scale,
                num_classes=1000,
                is_train=False,
                learn_all=False,
                is_imagenet=True
                )

        if exists(join(results_path, 'imagenet_{}.pickle'.format(num))):
            with open(join(results_path, 'imagenet_{}.pickle'.format(num)), 'rb') as handle:
                results_imagenet = pickle.load(handle)
        else:
            snapshot_prefix = 'snapshots/imagenet/from_scratch/imagenet_lmdb_{}_perclass'.format(num)
            results_imagenet = train_net(create_solver_params(imagenet_finetune, test_netspec=imagenet_test, max_iter=iters, test_interv=iters,
                                                              base_lr=0.001, snapshot_prefix=snapshot_prefix),
                                         loss_blobs=loss_blobs_f, acc_blobs=acc_blobs_f)
            acc['AlexNet'][num] = results_imagenet['acc'][acc_blobs_test[0]][0]
            with open(join(results_path, 'imagenet_{}.pickle'.format(num)), 'wb') as handle:
                pickle.dump(results_imagenet, handle, protocol=pickle.HIGHEST_PROTOCOL)

        # EGOMOTION
        snapshot_prefix = 'snapshots/imagenet/egomotion_finetuning/imagenet_lmdb_{}_perclass'.format(num)
        results_egomotion = train_net(create_solver_params(imagenet_finetune, test_netspec=imagenet_test, max_iter=iters, test_interv=iters,
                                                           base_lr=0.0001, snapshot_prefix=snapshot_prefix),
                                      loss_blobs=loss_blobs_f, acc_blobs=acc_blobs_f, pretrained_weights=results_ego['best_snap'])
        acc['egomotion'][num] = results_egomotion['acc'][acc_blobs_test[0]][0]

        # CONTRASTIVE m=10
        snapshot_prefix = 'snapshots/imagenet/contrastive10_finetuning/imagenet_lmdb_{}_perclass'.format(num)
        results_contrastive10 = train_net(create_solver_params(imagenet_finetune, test_netspec=imagenet_test, max_iter=iters, test_interv=iters, base_lr=0.0001, snapshot_prefix=snapshot_prefix),
                                          loss_blobs=loss_blobs_f, acc_blobs=acc_blobs_test, pretrained_weights=results_contr10['best_snap'])
        acc['cont_10'][num] = results_contrastive10['acc'][acc_blobs_test[0]][0]

    print('Accuracies')
    print('method\t' + '    \t'.join(sizes_lmdb))
    for a in ['cont_10', 'egomotion', 'AlexNet']:
        res = a
        for num in sizes_lmdb:
            res += "    \t" + "{0:.3f}".format(acc[a][num])
        print(res)

