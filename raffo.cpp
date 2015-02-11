#include <math.h>
#include "raffo.peg"
#include <lv2plugin.hpp>
#include <lv2_event_helpers.h>
#include <lv2_uri_map.h>
#include <lv2types.hpp>
#include <stdlib.h>

#include <list>
#include <iostream>

// cantidad maxima de samples que se procesan por llamado a render()
// un numero mayor resulta en mejor performance, pero peor granularidad en la transición de frecuencias
#define max_samples 256

using namespace std;


static inline float key2hz(unsigned char key) {
  return 8.1758 * pow(1.0594, key);
}

float min_fact(float a, float b) {
  return ((fabs(a-1) > fabs(b-1))? b: a);
}

float envelope(int count, float a, float d, float s, float c1, float c2) { // zapato: esto seria mas eficiente con un define?
  return (s - c1 * (count - a - d - fabs(count - a - d)) + 
         (c2 + c1) * (count - a - fabs(count - a))) ;
}

class RaffoSynth : public LV2::Plugin<RaffoSynth, LV2::URIMap<true> > //LV2::Synth<RaffoVoice, RaffoSynth> 
{
  
protected:
  /*float*& p(uint32_t port) {
    return reinterpret_cast<float*&>(Parent::m_ports[port]);
  }*/
  
  
  
  double sample_rate;
  list<unsigned char> keys;
  uint32_t period; // periodo de la nota presionada
  float glide_period; // periodo que se esta reproduciendo
  float last_val;
  uint32_t counter;
  int envelope_count;
  float modwheel;
  float pitch;
  
  double glide;
  
  uint32_t midi_type;
  

public:
  typedef LV2::Plugin<RaffoSynth, LV2::URIMap<true> > Parent;
  
  RaffoSynth(double rate): 
    Parent(m_n_ports),
    sample_rate(rate),
    period(500),
    counter(0),
    pitch(1)
    {
      midi_type = Parent::uri_to_id(LV2_EVENT_URI, "http://lv2plug.in/ns/ext/midi#MidiEvent"); 
    }
     
     
  void render(uint32_t from, uint32_t to) {
    if (keys.empty()) return;
    
    // buffer en 0
    for (uint32_t i = from; i < to; ++i) p(m_output)[i] = 0;
    
    double glide_factor;
    if (*p(m_glide) < .1) {
      glide_period = period;
      glide_factor = 1;
    } else {
      glide = pow(2., (to-from) / (sample_rate * (*p(m_glide)/5.))) ;
      glide_factor = min_fact(((glide_period < period)? glide : 1. / glide), 
                           period/glide_period);
      glide_period *= glide_factor;
    }
    
    // osciladores
    int envelope_subcount;
    for (int osc = 0; osc < 4; osc++) {    
      envelope_subcount = envelope_count;
      float vol = pow(*p(m_volume) * *p(m_vol0 + osc) / 100., 2); // el volumen es el cuadrado de la amplitud
      float subperiod = glide_period / (pow(2, *p(m_range0 + osc)) * pitch); // periodo efectivo del oscilador
    
      // valores precalculados para el envelope
      // la función de envelope es:
        // f(t) = s - (1-s)/(2*d) * (t-a-d-|t-a-d|) + (1/(2*a) + (1-s)/(2*d)) * (t-a-|t-a|)
        /*
              /\
             /  \
            /    \_______________  -> s = sustain level
           /  
          /
          |-a-|-d-|--------------|
        */
      float a = *p(m_attack)*100 + .1;
      float d = *p(m_decay)*100 + .1;
      float s = pow(*p(m_sustain),2);
      float c1 = (1.-s)/(2.*d);
      float c2 = 1./(2.*a);

      counter = last_val * glide_period + 1;
      
      switch ((int)*p(m_wave0 + osc)) {
        case (0): { //triangular
          for (uint32_t i = from; i < to; ++i && counter++ && envelope_subcount++) {
            p(m_output)[i] += vol * (4. * (fabs(fmod(((counter) + subperiod/4.), subperiod) /
                              subperiod - .5)-.25)) * 
                              envelope(envelope_count, a, d, s, c1, c2);
          }
          // zapato: la onda triangular esta hecha para que empiece continua, pero cuando se corta popea
          break;
        }
        case (1): { //sierra
          for (uint32_t i = from; i < to; ++i && counter++ && envelope_subcount++) {
            p(m_output)[i] += vol * (2. * fmod(counter, subperiod) / subperiod - 1) * 
                              envelope(envelope_count, a, d, s, c1, c2);
          
          }
          break;
        }
        case (2): { //cuadrada
          for (uint32_t i = from; i < to; ++i && counter++ && envelope_subcount++) {
            p(m_output)[i] += vol * (2. * ((fmod(counter, subperiod) / subperiod - .5) < 0)-1) * 
                              envelope(envelope_count, a, d, s, c1, c2);
          }
          break;
        }
        case (3): { //pulso
          for (uint32_t i = from; i < to; ++i && counter++ && envelope_subcount++) {
            p(m_output)[i] += vol * (2. * ((fmod(counter, subperiod) / subperiod - .2) < 0)-1) * 
                              envelope(envelope_count, a, d, s, c1, c2);
          }
          break;
        }
      
      }
    }
    //counter = counter % (int)glide_period;
    envelope_count += to - from;
    last_val = fmod(counter, glide_period / pitch) * pitch / glide_period; //para ajustar el enganche de la onda entre corridas de la funcion
  }
  
  void handle_midi(uint32_t size, unsigned char* data) {
    if (size == 3) {
      switch (data[0]) {
        case (0x90): { // note on
          if (keys.empty()) {
            envelope_count = 0;
            glide_period = sample_rate * 4 / key2hz(data[1]);
            counter = 0;
          }
          keys.push_front(data[1]);
          period = sample_rate * 4 / key2hz(data[1]);
          break;
        }
        case (0x80): { // note off
          keys.remove(data[1]);
          period = sample_rate * 4 / key2hz(keys.front());
          break;
        }
        case (0xE0): { // pitch bend
          /* Calculamos el factor de pitch (numero por el que multiplicar 
             la frecuencia fundamental). data[2] es el byte mas significativo, 
             data[1] el menos. El primer bit de ambos es 0, por eso << 7. 
             pitch_width es el numero maximo de semitonos de amplitud del pitch.
          * Mas informacion: http://sites.uci.edu/camp2014/2014/04/30/managing-midi-pitchbend-messages/
          */
          pitch = pow(2.,(((data[2] << 7) ^ data[1]) / 8191. - 1) / 6.); 
        }  
      }
    }
  } /*handle_midi*/
  
  void run(uint32_t sample_count) {
    /*pitch += 0.001;
    counter = counter % period;
    */
    LV2_Event_Iterator iter;
    lv2_event_begin(&iter, reinterpret_cast<LV2_Event_Buffer*&>(Parent::m_ports[m_midi]));

    uint8_t* event_data;
    uint32_t samples_done = 0;

    while (samples_done < sample_count) {
      uint32_t to = sample_count;
      LV2_Event* ev = 0;
      if (lv2_event_is_valid(&iter)) {
        ev = lv2_event_get(&iter, &event_data);
        to = ev->frames;
        lv2_event_increment(&iter);
      }
      if (to > samples_done) {
        while (samples_done + max_samples < to) { // subdividimos el buffer en porciones de tamaño max_sample
          render(samples_done, samples_done + max_samples);
          samples_done += max_samples;
        }
        render(samples_done, to);
        samples_done = to;
      }

      if (ev) {
        if (ev->type == midi_type)
          static_cast<RaffoSynth*>(this)->handle_midi(ev->size, event_data);
      }
    }
  } /*run*/
  
};

static int _ = RaffoSynth::register_class(m_uri);

