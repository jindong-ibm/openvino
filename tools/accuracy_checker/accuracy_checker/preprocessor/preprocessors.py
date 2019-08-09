"""
Copyright (c) 2019 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""
import math
import cv2
import numpy as np
from PIL import Image

from ..config import BaseField, BoolField, ConfigValidator, NumberField, StringField, ConfigError
from ..dependency import ClassProvider
from ..utils import get_size_from_config, get_or_parse_value, string_to_tuple, get_size_3d_from_config


class BasePreprocessorConfig(ConfigValidator):
    type = StringField()


class Preprocessor(ClassProvider):
    __provider_type__ = 'preprocessor'
    _config_validator_type = BasePreprocessorConfig

    def __init__(self, config, name=None):
        self.config = config
        self.name = name

        self.validate_config()
        self.configure()

    def __call__(self, *args, **kwargs):
        return self.process(*args, **kwargs)

    def process(self, image, annotation_meta=None):
        raise NotImplementedError

    def configure(self):
        pass

    def validate_config(self):
        self._config_validator_type(
            self.name, on_extra_argument=self._config_validator_type.ERROR_ON_EXTRA_ARGUMENT
        ).validate(self.config)


def scale_width(dst_width, dst_height, image_width, image_height,):
    return int(dst_width * image_width / image_height), dst_height


def scale_height(dst_width, dst_height, image_width, image_height):
    return dst_width, int(dst_height * image_height / image_width)


def scale_greater(dst_width, dst_height, image_width, image_height):
    if image_height > image_width:
        return scale_height(dst_width, dst_height, image_width, image_height)
    return scale_width(dst_width, dst_height, image_width, image_height)


def scale_fit_to_window(dst_width, dst_height, image_width, image_height):
    im_scale = min(dst_height / image_height, dst_width / image_width)
    return int(im_scale * image_width), int(im_scale * image_height)


PILLOW_INTERPOLATION = {
    'NEAREST': Image.NEAREST,
    'NONE': Image.NONE,
    'BOX': Image.BOX,
    'BILINEAR': Image.BILINEAR,
    'LINEAR': Image.LINEAR,
    'HAMMING': Image.HAMMING,
    'BICUBIC': Image.BICUBIC,
    'CUBIC': Image.CUBIC,
    'LANCZOS': Image.LANCZOS,
    'ANTIALIAS': Image.ANTIALIAS,
}

OPENCV_INTERPOLATION = {
    'NEAREST': cv2.INTER_NEAREST,
    'LINEAR': cv2.INTER_LINEAR,
    'CUBIC': cv2.INTER_CUBIC,
    'AREA': cv2.INTER_AREA,
    'MAX': cv2.INTER_MAX,
    'BITS': cv2.INTER_BITS,
    'BITS2': cv2.INTER_BITS2,
    'LANCZOS4': cv2.INTER_LANCZOS4,
}

ASPECT_RATIO_SCALE = {
    'width': scale_width,
    'height': scale_height,
    'greater': scale_greater,
    'fit_to_window': scale_fit_to_window
}

class ResizeConfigValidator(BasePreprocessorConfig):
    size = NumberField(floats=False, optional=True, min_value=1)
    dst_width = NumberField(floats=False, optional=True, min_value=1)
    dst_height = NumberField(floats=False, optional=True, min_value=1)
    aspect_ratio_scale = StringField(choices=ASPECT_RATIO_SCALE.keys(), optional=True)
    interpolation = StringField(
        choices=set(PILLOW_INTERPOLATION) | set(OPENCV_INTERPOLATION), optional=True
    )
    use_pillow = BoolField(optional=True)


class Resize(Preprocessor):
    __provider__ = 'resize'
    _config_validator_type = ResizeConfigValidator

    def configure(self):
        self.dst_height, self.dst_width = get_size_from_config(self.config)
        self.use_pil = self.config.get('use_pillow', False)

        interpolation = self.config.get('interpolation', 'LINEAR')

        self.scaling_func = ASPECT_RATIO_SCALE.get(self.config.get('aspect_ratio_scale'))

        if self.use_pil and interpolation.upper() not in PILLOW_INTERPOLATION:
            raise ValueError("Incorrect interpolation option: {} for resize preprocessing".format(interpolation))
        if not self.use_pil and interpolation.upper() not in OPENCV_INTERPOLATION:
            raise ValueError("Incorrect interpolation option: {} for resize preprocessing".format(interpolation))

        if self.use_pil:
            self.interpolation = PILLOW_INTERPOLATION[interpolation]
        else:
            self.interpolation = OPENCV_INTERPOLATION[interpolation]

    def process(self, image, annotation_meta=None):
        data = image.data
        new_height, new_width = self.dst_height, self.dst_width

        def process_data(data, new_height, new_width, scale_func, use_pil, interpolation):
            if scale_func:
                image_h, image_w = data.shape[:2]
                new_width, new_height = self.scaling_func(self.dst_width, self.dst_height, image_w, image_h)

            image.metadata['preferable_width'] = max(new_width, self.dst_width)
            image.metadata['preferable_height'] = max(new_height, self.dst_height)

            if use_pil:
                data = Image.fromarray(data)
                data = data.resize((new_width, new_height), interpolation)
                data = np.array(data)
                return data

            data = cv2.resize(data, (new_width, new_height), interpolation=interpolation).astype(np.float32)
            if len(data.shape) == 2:
                data = np.expand_dims(data, axis=-1)

            return data

        image.data = (
            process_data(data, new_height, new_width, self.scaling_func, self.use_pil, self.interpolation)
            if not isinstance(data, list) else [
                process_data(data_fragment, new_height, new_width, self.scaling_func, self.use_pil, self.interpolation)
                for data_fragment in data
            ]
        )

        return image


class NormalizeConfigValidator(BasePreprocessorConfig):
    mean = BaseField(optional=True)
    std = BaseField(optional=True)

class Normalize(Preprocessor):
    __provider__ = 'normalization'

    PRECOMPUTED_MEANS = {
        'imagenet': (104.00698793, 116.66876762, 122.67891434),
        'cifar10': (125.307, 122.961, 113.8575),
    }

    PRECOMPUTED_STDS = {
        'imagenet': (104.00698793, 116.66876762, 122.67891434),
        'cifar10': (125.307, 122.961, 113.8575),
    }

    _config_validator_type = NormalizeConfigValidator

    def configure(self):
        self.mean = get_or_parse_value(self.config.get('mean'), Normalize.PRECOMPUTED_MEANS)
        self.std = get_or_parse_value(self.config.get('std'), Normalize.PRECOMPUTED_STDS)
        if not self.mean and not self.std:
            raise ConfigError('mean or std value should be provided')

        if self.std and 0 in self.std:
            raise ConfigError('std value should not contain 0')

        if self.mean and not (len(self.mean) == 3 or len(self.mean) == 1):
            raise ConfigError('mean should be one value or comma-separated list channel-wise values')

        if self.std and not (len(self.std) == 3 or len(self.std) == 1):
            raise ConfigError('std should be one value or comma-separated list channel-wise values')

    def process(self, image, annotation_meta=None):
        def process_data(data, mean, std):
            if self.mean:
                data = data - mean
            if self.std:
                data = data / std

            return data

        image.data = process_data(image.data, self.mean, self.std) if not isinstance(image.data, list) else [
            process_data(data_fragment, self.mean, self.std) for data_fragment in image.data
        ]

        return image


class BgrToRgb(Preprocessor):
    __provider__ = 'bgr_to_rgb'

    def process(self, image, annotation_meta=None):
        def process_data(data):
            return cv2.cvtColor(data, cv2.COLOR_BGR2RGB)
        image.data = process_data(image.data) if not isinstance(image.data, list) else [
            process_data(fragment) for fragment in image.data
        ]
        return image


class BgrToGray(Preprocessor):
    __provider__ = 'bgr_to_gray'

    def process(self, image, annotation_meta=None):
        image.data = np.expand_dims(cv2.cvtColor(image.data, cv2.COLOR_BGR2GRAY).astype(np.float32), -1)
        return image

FLIP_MODES = {
    'horizontal': 0,
    'vertical': 1
}


class FlipConfigValidator(BasePreprocessorConfig):
    mode = StringField(choices=FLIP_MODES.keys())


class Flip(Preprocessor):
    __provider__ = 'flip'

    _config_validator_type = FlipConfigValidator

    def configure(self):
        mode = self.config.get('mode', 'horizontal')
        if isinstance(mode, str):
            self.mode = FLIP_MODES[mode]

    def process(self, image, annotation_meta=None):
        image.data = cv2.flip(image.data, self.mode)
        return image


class CropConfigValidator(BasePreprocessorConfig):
    size = NumberField(floats=False, optional=True, min_value=1)
    dst_width = NumberField(floats=False, optional=True, min_value=1)
    dst_height = NumberField(floats=False, optional=True, min_value=1)
    use_pillow = BoolField(optional=True)


class Crop(Preprocessor):
    __provider__ = 'crop'
    _config_validator_type = CropConfigValidator

    def configure(self):
        self.use_pillow = self.config.get('use_pillow', False)
        self.dst_height, self.dst_width = get_size_from_config(self.config)

    def process(self, image, annotation_meta=None):
        data = image.data

        def process_data(data, dst_height, dst_width, use_pillow):
            height, width = data.shape[:2]
            if use_pillow:
                i = int(round((height - self.dst_height) / 2.))
                j = int(round((width - self.dst_width) / 2.))
                croped_data = Image.fromarray(data).crop((j, i, j + self.dst_width, i + self.dst_height))
                data = np.array(croped_data)
                return data

            if width < dst_width or height < dst_height:
                resized = np.array([width, height])
                if resized[0] < dst_width:
                    resized = resized * dst_width / resized[0]
                if resized[1] < dst_height:
                    resized = resized * dst_height / resized[1]

                data = cv2.resize(data, tuple(np.ceil(resized).astype(int)))

            height, width, _ = data.shape
            start_height = (height - dst_height) // 2
            start_width = (width - dst_width) // 2

            return data[start_height:start_height + dst_height, start_width:start_width + dst_width]

        image.data = process_data(
            data, self.dst_height, self.dst_width, self.use_pillow
        ) if not isinstance(data, list) else [
            process_data(fragment, self.dst_height, self.dst_width, self.use_pillow) for fragment in image.data
        ]
        return image


class CropRect(Preprocessor):
    __provider__ = 'crop_rect'

    def process(self, image, annotation_meta=None):
        rect = annotation_meta.get('rect')
        if not rect:
            return image

        rows, cols = image.data.shape[:2]
        rect_x_min, rect_y_min, rect_x_max, rect_y_max = rect
        start_width, start_height = max(0, rect_x_min), max(0, rect_y_min)

        width = min(start_width + (rect_x_max - rect_x_min), cols)
        height = min(start_height + (rect_y_max - rect_y_min), rows)

        image.data = image.data[start_height:height, start_width:width]
        return image


class ExtendAroundRectConfigValidator(BasePreprocessorConfig):
    augmentation_param = NumberField(floats=True, optional=True)


class ExtendAroundRect(Preprocessor):
    __provider__ = 'extend_around_rect'
    _config_validator_type = ExtendAroundRectConfigValidator

    def configure(self):
        self.augmentation_param = self.config.get('augmentation_param', 0)

    def process(self, image, annotation_meta=None):
        rect = annotation_meta.get('rect')
        rows, cols = image.data.shape[:2]

        rect_x_left, rect_y_top, rect_x_right, rect_y_bottom = rect or (0, 0, cols, rows)
        rect_x_left = max(0, rect_x_left)
        rect_y_top = max(0, rect_y_top)
        rect_x_right = min(rect_x_right, cols)
        rect_y_bottom = min(rect_y_bottom, rows)

        rect_w = rect_x_right - rect_x_left
        rect_h = rect_y_bottom - rect_y_top

        width_extent = (rect_x_right - rect_x_left + 1) * self.augmentation_param
        height_extent = (rect_y_bottom - rect_y_top + 1) * self.augmentation_param
        rect_x_left = rect_x_left - width_extent
        border_left = abs(min(0, rect_x_left))
        rect_x_left = int(max(0, rect_x_left))

        rect_y_top = rect_y_top - height_extent
        border_top = abs(min(0, rect_y_top))
        rect_y_top = int(max(0, rect_y_top))

        rect_y_bottom += border_top
        rect_y_bottom = int(rect_y_bottom + height_extent + 0.5)
        border_bottom = abs(max(0, rect_y_bottom - rows))

        rect_x_right += border_left
        rect_x_right = int(rect_x_right + width_extent + 0.5)
        border_right = abs(max(0, rect_x_right - cols))

        image.data = cv2.copyMakeBorder(
            image.data, int(border_top), int(border_bottom), int(border_left), int(border_right), cv2.BORDER_REPLICATE
        )

        rect = (
            int(rect_x_left), int(rect_y_top),
            int(rect_x_left) + int(rect_w + width_extent * 2), int(rect_y_top) + int(rect_h + height_extent * 2)
        )
        annotation_meta['rect'] = rect

        return image


class PointAlignerConfigValidator(BasePreprocessorConfig):
    draw_points = BoolField(optional=True)
    normalize = BoolField(optional=True)
    size = NumberField(floats=False, optional=True, min_value=1)
    dst_width = NumberField(floats=False, optional=True, min_value=1)
    dst_height = NumberField(floats=False, optional=True, min_value=1)

class PointAligner(Preprocessor):
    __provider__ = 'point_alignment'
    _config_validator_type = PointAlignerConfigValidator

    ref_landmarks = np.array([
        30.2946 / 96, 51.6963 / 112,
        65.5318 / 96, 51.5014 / 112,
        48.0252 / 96, 71.7366 / 112,
        33.5493 / 96, 92.3655 / 112,
        62.7299 / 96, 92.2041 / 112
    ], dtype=np.float64).reshape(5, 2)

    def configure(self):
        self.draw_points = self.config.get('draw_points', False)
        self.normalize = self.config.get('normalize', True)
        self.dst_height, self.dst_width = get_size_from_config(self.config)

    def process(self, image, annotation_meta=None):
        keypoints = annotation_meta.get('keypoints')
        image.data = self.align(image.data, keypoints)
        return image

    def align(self, img, points):
        if not points:
            return img

        points_number = len(points) // 2
        points = np.array(points).reshape(points_number, 2)

        inp_shape = [1., 1.]
        if self.normalize:
            inp_shape = img.shape

        keypoints = points.copy().astype(np.float64)
        keypoints[:, 0] *= (float(self.dst_width) / inp_shape[1])
        keypoints[:, 1] *= (float(self.dst_height) / inp_shape[0])

        keypoints_ref = np.zeros((points_number, 2), dtype=np.float64)
        keypoints_ref[:, 0] = self.ref_landmarks[:, 0] * self.dst_width
        keypoints_ref[:, 1] = self.ref_landmarks[:, 1] * self.dst_height

        transformation_matrix = self.transformation_from_points(np.array(keypoints_ref), np.array(keypoints))
        img = cv2.resize(img, (self.dst_width, self.dst_height))
        if self.draw_points:
            for point in keypoints:
                cv2.circle(img, (int(point[0]), int(point[1])), 5, (255, 0, 0), -1)

        return cv2.warpAffine(img, transformation_matrix, (self.dst_width, self.dst_height), flags=cv2.WARP_INVERSE_MAP)

    @staticmethod
    def transformation_from_points(points1, points2):
        points1 = np.matrix(points1.astype(np.float64))
        points2 = np.matrix(points2.astype(np.float64))

        c1 = np.mean(points1, axis=0)
        c2 = np.mean(points2, axis=0)
        points1 -= c1
        points2 -= c2
        s1 = np.std(points1)
        s2 = np.std(points2)
        points1 /= np.maximum(s1, np.finfo(np.float64).eps)
        points2 /= np.maximum(s1, np.finfo(np.float64).eps)
        points_std_ratio = s2 / np.maximum(s1, np.finfo(np.float64).eps)

        u, _, vt = np.linalg.svd(points1.T * points2)
        r = (u * vt).T

        return np.hstack((points_std_ratio * r, c2.T - points_std_ratio * r * c1.T))


def center_padding(dst_width, dst_height, width, height):
    pad = [int(math.floor((dst_height - height) / 2.0)), int(math.floor((dst_width - width) / 2.0))]
    pad.extend([dst_height - height - pad[0], dst_width - width - pad[1]])

    return pad


def right_bottom_padding(dst_width, dst_height, width, height):
    return [0, 0, dst_height - height, dst_width - width]


def left_top_padding(dst_width, dst_height, width, height):
    return [dst_height - height, dst_width - width, 0, 0]


padding_func = {
    'center': center_padding,
    'left_top': left_top_padding,
    'right_bottom': right_bottom_padding
}


class PaddingConfigValidator(BasePreprocessorConfig):
    stride = NumberField(floats=False, min_value=1, optional=True)
    pad_value = StringField(optional=True)
    size = NumberField(floats=False, optional=True, min_value=1)
    dst_width = NumberField(floats=False, optional=True, min_value=1)
    dst_height = NumberField(floats=False, optional=True, min_value=1)
    pad_type = StringField(choices=padding_func.keys(), optional=True)
    use_numpy = BoolField(optional=True)


class Padding(Preprocessor):
    __provider__ = 'padding'
    _config_validator_type = PaddingConfigValidator

    def configure(self):
        self.stride = self.config.get('stride', 1)
        pad_val = self.config.get('pad_value', '0,0,0')
        if isinstance(pad_val, int):
            self.pad_value = (pad_val, pad_val, pad_val)
        if isinstance(pad_val, str):
            self.pad_value = string_to_tuple(pad_val, int)
        self.dst_height, self.dst_width = get_size_from_config(self.config, allow_none=True)
        self.pad_func = padding_func[self.config.get('pad_type', 'center')]
        self.use_numpy = self.config.get('use_numpy', False)

    def process(self, image, annotation_meta=None):
        height, width, _ = image.data.shape
        pref_height = self.dst_height or image.metadata.get('preferable_height', height)
        pref_width = self.dst_width or image.metadata.get('preferable_width', width)
        height = min(height, pref_height)
        pref_height = math.ceil(pref_height / float(self.stride)) * self.stride
        pref_width = max(pref_width, width)
        pref_width = math.ceil(pref_width / float(self.stride)) * self.stride
        pad = self.pad_func(pref_width, pref_height, width, height)
        image.metadata['padding'] = pad
        padding_realization_func = self._opencv_padding if not self.use_numpy else self._numpy_padding
        image.data = padding_realization_func(image.data, pad)

        return image

    def _opencv_padding(self, image, pad):
        return cv2.copyMakeBorder(
            image, pad[0], pad[2], pad[1], pad[3], cv2.BORDER_CONSTANT, value=self.pad_value
        )

    def _numpy_padding(self, image, pad):
        pad_values = (
            (self.pad_value[0], self.pad_value[0]),
            (self.pad_value[1], self.pad_value[1]),
            (self.pad_value[2], self.pad_value[2])
        )
        return np.pad(
            image, ((pad[0], pad[2]), (pad[1], pad[3]), (0, 0)),
            mode='constant', constant_values=pad_values
        )


class TillingConfigValidator(BasePreprocessorConfig):
    margin = NumberField(floats=False, min_value=1)
    size = NumberField(floats=False, optional=True, min_value=1)
    dst_width = NumberField(floats=False, optional=True, min_value=1)
    dst_height = NumberField(floats=False, optional=True, min_value=1)


class Tiling(Preprocessor):
    __provider__ = 'tiling'
    _config_validator_type = TillingConfigValidator

    def configure(self):
        self.dst_height, self.dst_width = get_size_from_config(self.config)
        self.margin = self.config['margin']

    def process(self, image, annotation_meta=None):
        data = image.data
        image_size = data.shape
        output_height = self.dst_height - 2 * self.margin
        output_width = self.dst_width - 2 * self.margin
        data = cv2.copyMakeBorder(data, *np.full(4, self.margin), cv2.BORDER_REFLECT_101)
        num_tiles_h = image_size[0] // output_height + (1 if image_size[0] % output_height else 0)
        num_tiles_w = image_size[1] // output_width + (1 if image_size[1] % output_width else 0)
        tiled_data = []
        for height in range(num_tiles_h):
            for width in range(num_tiles_w):
                offset = [output_height * height, output_width * width]
                tile = data[offset[0]:offset[0] + self.dst_height, offset[1]:offset[1] + self.dst_width, :]
                margin = [0, self.dst_height - tile.shape[0], 0, self.dst_width - tile.shape[1]]
                tile = cv2.copyMakeBorder(tile, *margin, cv2.BORDER_REFLECT_101)
                tiled_data.append(tile)
        image.data = tiled_data
        image.metadata['tiles_shape'] = (num_tiles_h, num_tiles_w)
        image.metadata['multi_infer'] = True

        return image


class Crop3DConfigValidator(BasePreprocessorConfig):
    size = NumberField(floats=False, min_value=1)
    dst_width = NumberField(floats=False, optional=True, min_value=1)
    dst_height = NumberField(floats=False, optional=True, min_value=1)
    dst_volume = NumberField(floats=False, optional=True, min_value=1)


class Crop3D(Preprocessor):
    __provider__ = 'crop3d'
    _config_validator_type = Crop3DConfigValidator

    def configure(self):
        self.dst_height, self.dst_width, self.dst_volume = get_size_3d_from_config(self.config)

    def process(self, image, annotation_meta=None):
        image.data = self.crop_center(image.data, self.dst_height, self.dst_width, self.dst_volume)
        return image

    @staticmethod
    def crop_center(img, cropx, cropy, cropz):

        z, y, x, _ = img.shape

        # Make sure starting index is >= 0
        startx = max(x // 2 - (cropx // 2), 0)
        starty = max(y // 2 - (cropy // 2), 0)
        startz = max(z // 2 - (cropz // 2), 0)

        # Make sure ending index is <= size
        endx = min(startx + cropx, x)
        endy = min(starty + cropy, y)
        endz = min(startz + cropz, z)

        return img[startz:endz, starty:endy, startx:endx, :]


class Normalize3d(Preprocessor):
    __provider__ = "normalize3d"

    def process(self, image, annotation_meta=None):
        data = self.normalize_img(image.data)
        image_list = []
        for img in data:
            image_list.append(img)
        image.data = image_list
        image.metadata['multi_infer'] = True

        return image

    @staticmethod
    def normalize_img(img):
        for channel in range(img.shape[3]):
            channel_val = img[:, :, :, channel] - np.mean(img[:, :, :, channel])
            channel_val /= np.std(img[:, :, :, channel])
            img[:, :, :, channel] = channel_val

        return img
