#!/usr/bin/env python2.7
from collections import defaultdict
from os.path import join, exists
from os import makedirs
from optparse import OptionParser, OptionGroup
from utils.nets.cnn_factory import KITTINetFactory
from utils.solver.solver import train_net, create_solver_params

"""
Script to reproduce the KITTI results + SUN dataset.
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
            './experiment_kitti.py -L /media/eze/Datasets/KITTI/\\')
    parser.add_option_group(group_example)

    return parser.parse_args()


if __name__ == "__main__":
    (opts, args) = parse_options()

    acc = {'egomotion': {}, 
           'cont_10':{},
           'cont_100':{},
           'imag_20': {},
           'imag_1000': {}}

    scale = 1
    batch_size = 125 
    # Train AlexNet on ILSVRC'12 dataset with 20 and 1000 imgs per class
    base_lr = 0.001
    results_path = './results/kitti/'
    try:
        makedirs(results_path)
    except:
        pass

    snapshots_path = '/media/eze/Datasets/snapshots'
    try:
        makedirs(snapshots_path)
    except:
        pass

    # Imagenet 20 images per class
    iters = 20000 
    imagenet20, loss_blobs_imagenet, acc_blobs_imagenet = KITTINetFactory.standar(
            lmdb_path=join(opts.lmdb_root, 'ILSVRC12/ILSVRC12_Training_20perclass_lmdb'),
            mean_file='./datasets/data/mean_ilsvrc12.binaryproto',
            batch_size=batch_size,
            scale=scale,
            num_classes=1000,
            is_train=True,
            learn_all=True,
            is_imagenet=True
            )
    imagenet_test20, loss_blobs_test, acc_blobs_test = KITTINetFactory.standar(
            lmdb_path=join(opts.lmdb_root, 'ILSVRC12/ILSVRC12_Testing_20perclass_lmdb'),
            mean_file='./datasets/data/mean_ilsvrc12.binaryproto',
            batch_size=batch_size,
            scale=scale,
            num_classes=1000,
            is_train=False,
            learn_all=False,
            is_imagenet=True
            )
    snapshot_prefix = join(snapshots_path, 'imagenet/imagenet_lmdb20')
    results_imagenet20 = train_net(create_solver_params(imagenet20, test_netspec=imagenet_test20, max_iter=iters, test_interv=iters,
                                                       base_lr=base_lr, snapshot_prefix=snapshot_prefix),
                                  loss_blobs=loss_blobs_imagenet,
                                  acc_blobs=acc_blobs_imagenet,
                                  pickle_name=join(results_path, 'imagenet_20.pickle'))
    del imagenet20
    del imagenet_test20

    # Imagenet 1000 images per class
    iters = 80000
    imagenet1000, loss_blobs_imagenet, acc_blobs_imagenet = KITTINetFactory.standar(
            lmdb_path=join(opts.lmdb_root, 'ILSVRC12/ILSVRC12_Training_1000perclass_lmdb'),
            mean_file='./datasets/data/mean_ilsvrc12.binaryproto',
            batch_size=batch_size,
            scale=scale,
            num_classes=1000,
            is_train=True,
            learn_all=True,
            is_imagenet=True
            )
    imagenet_test1000, loss_blobs_test, acc_blobs_test = KITTINetFactory.standar(
            lmdb_path=join(opts.lmdb_root, 'ILSVRC12/ILSVRC12_Testing_1000perclass_lmdb'),
            mean_file='./datasets/data/mean_ilsvrc12.binaryproto',
            batch_size=batch_size,
            scale=scale,
            num_classes=1000,
            is_train=False,
            learn_all=False,
            is_imagenet=True
            )
    results_imagenet1000 = train_net(create_solver_params(imagenet1000, test_netspec=imagenet_test1000, max_iter=iters, test_interv=iters,
                                                       base_lr=base_lr, snapshot_prefix=join(snapshots_path, 'imagenet/imagenet_lmdb1000')),
                                  loss_blobs=loss_blobs_imagenet,
                                  acc_blobs=acc_blobs_imagenet,
                                  pickle_name=join(results_path, 'imagenet_80K_1000.pickle'))
    del imagenet1000
    del imagenet_test1000

    ## EGOMOTION NET
    ## Used to train a siamese network from scratch following the method from the
    ## paper
    batch_size = 60
    iters = 60000
    siam_kitti, loss_blobs, acc_blobs = KITTINetFactory.siamese_egomotion(
            lmdb_path=join(opts.lmdb_root, 'KITTI/kitti_train_egomotion_lmdb'),
            labels_lmdb_path=join(opts.lmdb_root, 'KITTI/kitti_train_egomotion_lmdb_labels'),
            mean_file='./datasets/data/mean_kitti_egomotion.binaryproto',
            batch_size=batch_size,
            scale=scale,
            is_train=True,
            learn_all=True
            )

    # Create a SolverParameter instance with the predefined parameters for this experiment.
    # Some paths and iterations numbers will change for nets with contrastive loss or
    # in finetuning stage
    # Train our first siamese net with Egomotion method
    results_ego = train_net(create_solver_params(siam_kitti, max_iter=iters, base_lr=base_lr, snapshot_prefix=join(snapshots_path, 'kitti/egomotion/kitti_siamese')),
            loss_blobs=loss_blobs,
            pickle_name=join(results_path, 'egomotion.pickle'))
    del siam_kitti

    # CONTRASTIVE NET, m=10
    # Using a small batch size while training with Contrastive Loss leads
    # to high bias in the networks (i.e. they dont learn much)
    # A good ad-hoc value is between 250-500
    iters=40000
    batch_size = 250 
    base_lr = 0.0001
    siam_cont10_kitti, loss_cont_blobs, acc_cont_blobs = KITTINetFactory.siamese_contrastive(
            lmdb_path=join(opts.lmdb_root, 'KITTI/kitti_train_sfa_lmdb'),
            mean_file='./datasets/data/mean_kitti_egomotion.binaryproto',
            batch_size=batch_size,
            scale=scale,
            contrastive_margin=10,
            is_train=True,
            learn_all=True
            )
    # Also, using a big lr (i.e. 0.01) while training with Contrastive Loss can lead to nan values while backpropagating the loss
    results_contr10 = train_net(create_solver_params(siam_cont10_kitti, max_iter=iters, base_lr=base_lr, snapshot_prefix=join(snapshots_path, 'kitti/contrastive/kitti_siamese_m10')),
            loss_blobs=loss_cont_blobs,
            pickle_name=join(results_path, 'contr_10.pickle'))
    del siam_cont10_kitti


    sizes_lmdb = ['5', '20']
    splits = ['01', '02', '03']
    outputs_to_test = ['1', '2', '3', '4', '5']
    iters = 10000
    batch_size = 125
    for output in outputs_to_test:
        for k in acc:
            acc[k][output] = defaultdict(int)
        for num in sizes_lmdb:
            for split in splits:
                # Finetune network
                kitti_finetune, loss_blobs_f, acc_blobs_f = KITTINetFactory.standar(
                        lmdb_path=join(opts.lmdb_root, 'SUN397/lmdbs/SUN_Training_{}_{}perclass_lmdb'.format(split, num)),
                        mean_file='./datasets/data/mean_ilsvrc12.binaryproto',
                        batch_size=batch_size,
                        scale=scale,
                        num_classes=397,
                        is_train=True,
                        learn_all=False,
                        layers=output
                        )

                # Test Net Used to test accuracy in finetunig stages
                kitti_test, loss_blobs_test, acc_blobs_test = KITTINetFactory.standar(
                        lmdb_path=join(opts.lmdb_root, 'SUN397/lmdbs/SUN_Testing_{}_{}perclass_lmdb'.format(split, num)),
                        mean_file='./datasets/data/mean_ilsvrc12.binaryproto',
                        batch_size=batch_size,
                        scale=scale,
                        num_classes=397,
                        is_train=False,
                        learn_all=False,
                        layers=output
                        )

                # EGOMOTION
                snapshot_prefix = join(snapshots_path, 'kitti/egomotion_finetuning/kitti_lmdb{}_outputL{}_split{}'.format(num, output, split))
                results_egomotion = train_net(create_solver_params(kitti_finetune, test_netspec=kitti_test, max_iter=iters, test_interv=iters,
                                                                   base_lr=base_lr, snapshot=iters, snapshot_prefix=snapshot_prefix),
                                              loss_blobs=loss_blobs_f,
                                              acc_blobs=acc_blobs_f,
                                              pretrained_weights=results_ego['best_snap'],
                                              pickle_name=join(results_path, 'egomotion_finetuning_layer{}_lmdb{}perclass_split{}.pickle'.format(output, num, split)))
                acc['egomotion'][output][num] += results_egomotion['acc'][acc_blobs_test[0]][0]

                ## CONTRASTIVE m=10
                snapshot_prefix = join(snapshots_path, 'kitti/contrastive10_finetuning/kitti_lmdb{}_outputL{}_split{}'.format(num, output, split))
                results_contrastive10 = train_net(create_solver_params(kitti_finetune, test_netspec=kitti_test, max_iter=iters, test_interv=iters, base_lr=base_lr, snapshot=iters, snapshot_prefix=snapshot_prefix),
                                                  loss_blobs=loss_blobs_f, 
                                                  acc_blobs=acc_blobs_test,
                                                  pretrained_weights=results_contr10['best_snap'],
                                                  pickle_name=join(results_path, 'contrastive_m10_finetuning_layer{}_lmdb{}perclass_split{}.pickle'.format(output, num, split)))
                acc['cont_10'][output][num] += results_contrastive10['acc'][acc_blobs_test[0]][0]

                ##Imagenet 20
                snapshot_prefix = join(snapshots_path, 'kitti/imagenet20_finetuning/kitti_lmdb{}_outputL{}_split{}'.format(num, output, split))
                results_finet_imagenet20 = train_net(create_solver_params(kitti_finetune, test_netspec=kitti_test, max_iter=iters, base_lr=base_lr, snapshot=iters, snapshot_prefix=snapshot_prefix),
                                                   loss_blobs=loss_blobs_f,
                                                   acc_blobs=acc_blobs_test,
                                                   pretrained_weights=join(results_path, results_imagenet20['best_snap']),
                                                   pickle_name=join(results_path, 'imagenet20perclass_finetuning_layer{}_lmdb{}perclass_split{}.pickle'.format(output, num, split)))
                acc['imag_20'][output][num] += results_finet_imagenet20['acc'][acc_blobs_test[0]][0]

                ##Imagenet 1000
                snapshot_prefix = join(snapshots_path, 'kitti/imagenet1000_finetuning/kitti_lmdb{}_outputL{}_split{}'.format(num, output, split))
                results_finet_imagenet1000 = train_net(create_solver_params(kitti_finetune, test_netspec=kitti_test, max_iter=iters, base_lr=base_lr, snapshot=iters, snapshot_prefix=snapshot_prefix),
                                                   loss_blobs=loss_blobs_f,
                                                   acc_blobs=acc_blobs_test,
                                                   pretrained_weights=join(results_path, results_imagenet1000['best_snap']),
                                                   pickle_name=join(results_path, 'imagenet1000perclass_finetuning_80K_layer{}_lmdb{}perclass_split{}.pickle'.format(output, num, split)))
                acc['imag_1000'][output][num] += results_finet_imagenet1000['acc'][acc_blobs_test[0]][0]

                del kitti_finetune
                del kitti_test

            for k in acc:
                acc[k][output][num] = acc[k][output][num] / float(len(splits))

    print('Accuracies')
    for k in acc:
        res = k
        for num in sizes_lmdb:
            res += "   \t {}\t".format(num)
            for out in outputs_to_test:
                res += "    \t" + "{0:.2f}".format(acc[k][out][num] * 100.0)
        print(res)
